/*
 * locate.c -- Get current geographic coordinates via native platform APIs
 *
 * macOS:  CoreLocation (Obj-C) -- binary must run from inside .app bundle
 *         so macOS shows the permission dialog.
 * Linux:  GeoClue2 D-Bus via sd-bus (libsystemd).
 *
 * Dual-mode:
 *   Ext mode  (stdin is pipe): JSON-RPC 2.0 request -> response
 *   CLI mode  (stdin is TTY):  plain JSON to stdout
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <signal.h>

#define TIMEOUT_SEC 30
#define LOCATE_PI 3.14159265358979323846
#define LOCATE_A 6378245.0
#define LOCATE_EE 0.00669342162296594323

/* ----------------------------------------------------------------- */
/*  Tiny JSON helpers (no external deps)                             */
/* ----------------------------------------------------------------- */

static int json_get_int(const char *s, const char *key, int *val)
{
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(s, pat);
	if (!p) return -1;
	p = strchr(p + strlen(key) + 2, ':');
	if (!p) return -1;
	while (*p && (*p < '0' || *p > '9') && *p != '-') p++;
	*val = (int)strtol(p, NULL, 10);
	return 0;
}

/* ----------------------------------------------------------------- */
/*  Coordinate system conversions                                    */
/* ----------------------------------------------------------------- */

static int coord_in_china(double lat, double lon)
{
	return lon >= 72.004 && lon <= 137.8347 &&
	       lat >= 0.8293 && lat <= 55.8271;
}

static double transform_lat(double x, double y)
{
	double ret;

	ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y +
	      0.1 * x * y + 0.2 * sqrt(fabs(x));
	ret += (20.0 * sin(6.0 * x * LOCATE_PI) +
		20.0 * sin(2.0 * x * LOCATE_PI)) * 2.0 / 3.0;
	ret += (20.0 * sin(y * LOCATE_PI) +
		40.0 * sin(y / 3.0 * LOCATE_PI)) * 2.0 / 3.0;
	ret += (160.0 * sin(y / 12.0 * LOCATE_PI) +
		320.0 * sin(y * LOCATE_PI / 30.0)) * 2.0 / 3.0;
	return ret;
}

static double transform_lon(double x, double y)
{
	double ret;

	ret = 300.0 + x + 2.0 * y + 0.1 * x * x +
	      0.1 * x * y + 0.1 * sqrt(fabs(x));
	ret += (20.0 * sin(6.0 * x * LOCATE_PI) +
		20.0 * sin(2.0 * x * LOCATE_PI)) * 2.0 / 3.0;
	ret += (20.0 * sin(x * LOCATE_PI) +
		40.0 * sin(x / 3.0 * LOCATE_PI)) * 2.0 / 3.0;
	ret += (150.0 * sin(x / 12.0 * LOCATE_PI) +
		300.0 * sin(x / 30.0 * LOCATE_PI)) * 2.0 / 3.0;
	return ret;
}

static void wgs84_to_gcj02(double lat, double lon,
			   double *out_lat, double *out_lon)
{
	double dlat;
	double dlon;
	double radlat;
	double magic;
	double sqrtmagic;

	if (!coord_in_china(lat, lon)) {
		*out_lat = lat;
		*out_lon = lon;
		return;
	}

	dlat = transform_lat(lon - 105.0, lat - 35.0);
	dlon = transform_lon(lon - 105.0, lat - 35.0);
	radlat = lat / 180.0 * LOCATE_PI;
	magic = sin(radlat);
	magic = 1.0 - LOCATE_EE * magic * magic;
	sqrtmagic = sqrt(magic);
	dlat = (dlat * 180.0) /
	       ((LOCATE_A * (1.0 - LOCATE_EE)) /
		(magic * sqrtmagic) * LOCATE_PI);
	dlon = (dlon * 180.0) /
	       (LOCATE_A / sqrtmagic * cos(radlat) * LOCATE_PI);
	*out_lat = lat + dlat;
	*out_lon = lon + dlon;
}

/* ----------------------------------------------------------------- */
/*  Platform: macOS  --  CoreLocation                                */
/* ----------------------------------------------------------------- */

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <CoreLocation/CoreLocation.h>
#import <objc/runtime.h>

/*
 * CoreLocation delivers delegate callbacks on the main run loop.
 * We use CFRunLoopRun() with a CFRunLoopTimer -- NOT dispatch_after --
 * so the run loop processes events and the callbacks actually fire.
 *
 * g_loc_delegate is a file-scope global so the CFRunLoopTimer
 * callback can access it without passing an ObjC object pointer
 * through a C function context (which triggers SIGBUS on arm64).
 *
 * IMPORTANT:  For the permission dialog to appear, the binary MUST
 * reside inside a .app bundle (Locate.app/Contents/MacOS/locate).
 * Standalone CLI binaries are silently ignored by macOS >= Ventura.
 *
 * Property access on LocDel (d.loc, d.mgr, etc.) is avoided after
 * CFRunLoopRun returns because CoreLocation's delegate callbacks
 * may have been invoked inside an autorelease pool that has since
 * been drained.  Objects stored via @property (strong) are safe
 * only while the pool that created them is alive; accessing them
 * after the pool is drained triggers objc_retain on a zombie.
 * We work around this by reading ivars directly via
 * object_getInstanceVariable / ivar_getOffset, bypassing the
 * retaining property getter entirely.
 */

@class LocDel;
static LocDel *g_loc_delegate;

@interface LocDel : NSObject <CLLocationManagerDelegate>
@property (strong) CLLocationManager *mgr;
@property (strong) CLLocation *loc;
@property (strong) NSError *err;
@property int timedout;
@end

@implementation LocDel
- (id)init
{
	if ((self = [super init])) {
		_mgr = [[CLLocationManager alloc] init];
		_mgr.delegate = self;
		_mgr.desiredAccuracy = kCLLocationAccuracyBest;
	}
	return self;
}

- (void)locationManager:(CLLocationManager *)m
	didUpdateLocations:(NSArray<CLLocation *> *)locs
{
	_loc = locs.lastObject;
	[_loc retain]; /* prevent drain in autorelease pool */
	[m stopUpdatingLocation];
	CFRunLoopStop(CFRunLoopGetCurrent());
}

- (void)locationManager:(CLLocationManager *)m
	didFailWithError:(NSError *)error
{
	_err = [error retain]; /* prevent drain in autorelease pool */
	CFRunLoopStop(CFRunLoopGetCurrent());
}

- (void)locationManagerDidChangeAuthorization:(CLLocationManager *)m
{
	/*
	 * IMPORTANT: Do NOT call requestAlwaysAuthorization from here.
	 * The delegate fires once on init with the CURRENT status.
	 * Calling requestAlwaysAuthorization re-entrantly causes
	 * infinite recursion and a segfault.
	 *
	 * requestAlwaysAuthorization is called once from get_location()
	 * below.  When the user responds to the dialog, this delegate
	 * fires again with the updated status.
	 */
	switch (m.authorizationStatus) {
	case kCLAuthorizationStatusAuthorizedAlways:
		[m startUpdatingLocation];
		return;
	case kCLAuthorizationStatusNotDetermined:
		/* Awaiting user response -- do nothing */
		return;
	default:
		/* Denied or Restricted */
		_err = [NSError errorWithDomain:NSCocoaErrorDomain
					code:1 userInfo:nil];
		CFRunLoopStop(CFRunLoopGetCurrent());
	}
}
@end

/*
 * Read an Objective-C object ivar directly, bypassing the
 * retaining property getter.  After CFRunLoopRun() returns,
 * the autorelease pools created by CoreLocation delegate
 * callbacks may have been drained, making the @property
 * accessors unsafe (they call objc_retain on a zombie).
 * Direct ivar reads are safe because we own the LocDel
 * instance and its ivars remain valid.
 */
static id ivar_read(LocDel *obj, const char *name)
{
	Ivar iv = class_getInstanceVariable(object_getClass(obj), name);
	if (!iv) return nil;
	return *(id *)((char *)(__bridge void *)obj + ivar_getOffset(iv));
}

static void timeout_timer_cb(CFRunLoopTimerRef timer, void *info)
{
	(void)timer;
	(void)info;
	g_loc_delegate.timedout = 1;
	CFRunLoopStop(CFRunLoopGetCurrent());
}

static void stop_runcb(CFRunLoopTimerRef timer, void *info)
{
	(void)timer;
	(void)info;
	CFRunLoopStop(CFRunLoopGetCurrent());
}

static int get_location(double *lat, double *lon,
			double *acc, double *alt,
			char *ts, size_t ts_sz)
{
	@autoreleasepool {
		LocDel *d = [[LocDel alloc] init];
		if (!d) return -ENOMEM;
		g_loc_delegate = d;

		CLAuthorizationStatus st = [d.mgr authorizationStatus];

		if (st == kCLAuthorizationStatusDenied ||
		    st == kCLAuthorizationStatusRestricted) {
			return -EPERM;
		}

		if (st == kCLAuthorizationStatusNotDetermined)
			[d.mgr requestAlwaysAuthorization];
		else
			[d.mgr startUpdatingLocation];

		/*
		 * Use CFRunLoopTimer as a timeout instead of dispatch_after.
		 * dispatch_after requires a fully-initialised libdispatch
		 * main queue which may not exist in a forked subprocess.
		 * CFRunLoopTimer works directly with the CFRunLoop and is
		 * reliable even without NSApplication / dispatch setup.
		 */
		CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
			kCFAllocatorDefault,
			CFAbsoluteTimeGetCurrent() + TIMEOUT_SEC,
			0, 0, 0,
			timeout_timer_cb,
			NULL);
		CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer,
				  kCFRunLoopDefaultMode);

		CFRunLoopRun(); /* blocks until CFRunLoopStop */

		CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer,
				      kCFRunLoopDefaultMode);
		CFRelease(timer);

		if (d.timedout) return -ETIMEDOUT;

		/*
		 * Read loc and err via direct ivar access, NOT property
		 * accessors.  CoreLocation delegate callbacks may have
		 * been invoked inside an autorelease pool that is now
		 * drained; the retaining getter on a (strong) @property
		 * would call objc_retain on a dangling pointer.
		 */
		NSError *err = (NSError *)ivar_read(d, "_err");
		CLLocation *loc = (CLLocation *)ivar_read(d, "_loc");

		if (err) return -EPERM;
		if (!loc) return -ENODATA;

		CLLocationCoordinate2D c = [loc coordinate];
		*lat = c.latitude;
		*lon = c.longitude;
		*acc = [loc horizontalAccuracy];
		*alt = [loc altitude];

		NSDateFormatter *fmt = [[NSDateFormatter alloc] init];
		fmt.dateFormat = @"yyyy-MM-dd'T'HH:mm:ssZZZZZ";
		NSString *s = [fmt stringFromDate:[loc timestamp]];
		[s getCString:ts maxLength:(NSUInteger)ts_sz
		     encoding:NSUTF8StringEncoding];

		return (isfinite(*lat) && isfinite(*lon)) ? 0 : -ENODATA;
	}
}

/* ----------------------------------------------------------------- */
/*  Platform: Linux  --  GeoClue2 via D-Bus (sd-bus)                 */
/* ----------------------------------------------------------------- */

#elif defined(__linux__)

#include <systemd/sd-bus.h>

static int get_location(double *lat, double *lon,
			double *acc, double *alt,
			char *ts, size_t ts_sz)
{
	sd_bus *bus = NULL;
	sd_bus_error error = SD_BUS_ERROR_NULL;
	sd_bus_message *reply = NULL;
	int rc;

	rc = sd_bus_open_system(&bus);
	if (rc < 0) return rc;

	/* ---- GeoClue2: GetClient ---- */
	rc = sd_bus_call_method(bus,
		"org.freedesktop.GeoClue2",
		"/org/freedesktop/GeoClue2/Manager",
		"org.freedesktop.GeoClue2.Manager",
		"GetClient",
		&error, &reply, "");
	if (rc < 0) goto out;

	const char *client_path = NULL;
	rc = sd_bus_message_read(reply, "o", &client_path);
	sd_bus_message_unref(reply);
	reply = NULL;
	if (rc < 0 || !client_path) goto out;

	/* ---- Set DesktopId ---- */
	rc = sd_bus_set_property(bus,
		"org.freedesktop.GeoClue2",
		client_path,
		"org.freedesktop.GeoClue2.Client",
		"DesktopId",
		&error, "s", "morph");
	if (rc < 0) goto out;

	/* ---- Start ---- */
	rc = sd_bus_call_method(bus,
		"org.freedesktop.GeoClue2",
		client_path,
		"org.freedesktop.GeoClue2.Client",
		"Start",
		&error, NULL, "");
	if (rc < 0) goto out;

	/* ---- Wait a moment for GeoClue2 to acquire a fix ---- */
	usleep(500000); /* 500 ms */

	/* ---- Get Location object path ---- */
	rc = sd_bus_get_property(bus,
		"org.freedesktop.GeoClue2",
		client_path,
		"org.freedesktop.GeoClue2.Client",
		"Location",
		&error, &reply, "o");
	if (rc < 0) goto out;

	const char *loc_path = NULL;
	rc = sd_bus_message_read(reply, "o", &loc_path);
	sd_bus_message_unref(reply);
	reply = NULL;
	if (rc < 0 || !loc_path || strcmp(loc_path, "/") == 0) {
		rc = -ENODATA;
		goto out;
	}

	/* ---- Read Latitude ---- */
	rc = sd_bus_get_property(bus,
		"org.freedesktop.GeoClue2",
		loc_path,
		"org.freedesktop.GeoClue2.Location",
		"Latitude",
		&error, &reply, "d");
	if (rc < 0) goto out;
	sd_bus_message_read(reply, "d", lat);
	sd_bus_message_unref(reply);
	reply = NULL;

	/* ---- Read Longitude ---- */
	rc = sd_bus_get_property(bus,
		"org.freedesktop.GeoClue2",
		loc_path,
		"org.freedesktop.GeoClue2.Location",
		"Longitude",
		&error, &reply, "d");
	if (rc < 0) goto out;
	sd_bus_message_read(reply, "d", lon);
	sd_bus_message_unref(reply);
	reply = NULL;

	/* ---- Read Accuracy (optional) ---- */
	*acc = 0;
	rc = sd_bus_get_property(bus,
		"org.freedesktop.GeoClue2",
		loc_path,
		"org.freedesktop.GeoClue2.Location",
		"Accuracy",
		&error, &reply, "d");
	if (rc == 0) {
		sd_bus_message_read(reply, "d", acc);
		sd_bus_message_unref(reply);
		reply = NULL;
	}

	/* ---- Read Altitude (optional) ---- */
	*alt = 0;
	sd_bus_error_free(&error);
	rc = sd_bus_get_property(bus,
		"org.freedesktop.GeoClue2",
		loc_path,
		"org.freedesktop.GeoClue2.Location",
		"Altitude",
		&error, &reply, "d");
	if (rc == 0) {
		sd_bus_message_read(reply, "d", alt);
		sd_bus_message_unref(reply);
		reply = NULL;
	}

	/* ---- Timestamp ---- */
	time_t now = time(NULL);
	struct tm utc;
	gmtime_r(&now, &utc);
	strftime(ts, ts_sz, "%Y-%m-%dT%H:%M:%SZ", &utc);

	rc = 0;

out:
	sd_bus_error_free(&error);
	if (reply) sd_bus_message_unref(reply);
	if (bus) sd_bus_unref(bus);
	return rc;
}

/* ----------------------------------------------------------------- */
/*  Platform: unknown                                                */
/* ----------------------------------------------------------------- */

#else

static int get_location(double *lat, double *lon,
			double *acc, double *alt,
			char *ts, size_t ts_sz)
{
	(void)lat; (void)lon; (void)acc;
	(void)alt; (void)ts; (void)ts_sz;
	return -ENOSYS;
}

#endif

/* ----------------------------------------------------------------- */
/*  JSON-RPC response builders                                       */
/* ----------------------------------------------------------------- */

static char *build_result(int id, double lat, double lon,
			  double acc, double alt, const char *ts)
{
	double out_lat;
	double out_lon;
	double gcj_lat;
	double gcj_lon;
	int in_china;
	char *buf = malloc(1024);

	if (!buf) return NULL;
	wgs84_to_gcj02(lat, lon, &gcj_lat, &gcj_lon);
	in_china = coord_in_china(lat, lon);
	out_lat = in_china ? gcj_lat : lat;
	out_lon = in_china ? gcj_lon : lon;

	snprintf(buf, 1024,
		"{"
		"\"jsonrpc\":\"2.0\","
		"\"id\":%d,"
		"\"result\":{"
		"\"latitude\":%.6f,"
		"\"longitude\":%.6f,"
		"\"region\":\"%s\","
		"\"accuracy_m\":%.1f,"
		"\"altitude\":%.1f,"
		"\"timestamp\":\"%s\""
		"}"
		"}",
		id, out_lat, out_lon, in_china ? "CN" : "GLOBAL", acc, alt,
		ts ? ts : "");
	return buf;
}

static void print_plain_result(double lat, double lon,
			       double acc, double alt, const char *ts)
{
	double out_lat;
	double out_lon;
	double gcj_lat;
	double gcj_lon;
	int in_china;

	wgs84_to_gcj02(lat, lon, &gcj_lat, &gcj_lon);
	in_china = coord_in_china(lat, lon);
	out_lat = in_china ? gcj_lat : lat;
	out_lon = in_china ? gcj_lon : lon;

	printf("{\"latitude\":%.6f,"
	       "\"longitude\":%.6f,"
	       "\"region\":\"%s\","
	       "\"accuracy_m\":%.1f,"
	       "\"altitude\":%.1f,"
	       "\"timestamp\":\"%s\"}\n",
	       out_lat, out_lon, in_china ? "CN" : "GLOBAL", acc, alt,
	       ts ? ts : "");
}

static char *build_error(int id, const char *msg)
{
	char *buf = malloc(256);
	if (!buf) return NULL;
	snprintf(buf, 256,
		"{"
		"\"jsonrpc\":\"2.0\","
		"\"id\":%d,"
		"\"error\":{\"code\":-1,\"message\":\"%s\"}"
		"}",
		id, msg);
	return buf;
}

/* ----------------------------------------------------------------- */
/*  Main                                                             */
/* ----------------------------------------------------------------- */

int main(int argc, char **argv)
{
	if (argc > 1 && (strcmp(argv[1], "-h") == 0 ||
	                 strcmp(argv[1], "--help") == 0)) {
		printf("Usage: locate [--request-permission]\n"
		       "Get current geographic coordinates.\n"
		       "\n"
		       "CLI mode (stdin is TTY):\n"
		       "  prints JSON with latitude, longitude, region,\n"
		       "  accuracy_m, altitude, and timestamp to stdout.\n"
		       "\n"
		       "Ext mode (stdin is pipe):\n"
		       "  reads a JSON-RPC 2.0 request from stdin and\n"
		       "  responds with coordinates via stdout.\n"
#if defined(__APPLE__)
		       "\n"
		       "macOS first-time setup:\n"
		       "  1. Run: locate --request-permission\n"
		       "  2. Click \"Allow\" in the dialog\n"
		       "  3. Re-run: locate\n"
#endif
		       );
		return 0;
	}

#if defined(__APPLE__)
	if (argc > 1 && strcmp(argv[1], "--request-permission") == 0) {
		@autoreleasepool {
			CLLocationManager *mgr = [[CLLocationManager alloc] init];
			[mgr requestAlwaysAuthorization];
			CFRunLoopTimerRef timer = CFRunLoopTimerCreate(
				kCFAllocatorDefault,
				CFAbsoluteTimeGetCurrent() + 2.0,
				0, 0, 0,
				stop_runcb, NULL);
			CFRunLoopAddTimer(CFRunLoopGetCurrent(), timer,
					  kCFRunLoopDefaultMode);
			CFRunLoopRun();
			CFRunLoopRemoveTimer(CFRunLoopGetCurrent(), timer,
					      kCFRunLoopDefaultMode);
			CFRelease(timer);
		}
		printf("Permission requested.\n"
		       "Check System Settings > Privacy & Security\n"
		       "> Location Services and enable locate.\n");
		return 0;
	}
#endif

	(void)argc;
	(void)argv;

	int id = 1;
	int is_ext = !isatty(STDIN_FILENO);

	/*
	 * In ext mode, read the JSON-RPC request FIRST, then get location.
	 * The caller (morph) writes the request and closes stdin; we must
	 * drain stdin before blocking on CoreLocation / GeoClue2.
	 */
	if (is_ext) {
		char req[4096];
		ssize_t n = read(STDIN_FILENO, req, sizeof(req) - 1);
		if (n > 0) {
			req[n] = '\0';
			json_get_int(req, "id", &id);
		}
	}

	double lat = 0.0, lon = 0.0, acc = 0.0, alt = 0.0;
	char timestamp[64];
	memset(timestamp, 0, sizeof(timestamp));

	int rc = get_location(&lat, &lon, &acc, &alt,
			      timestamp, sizeof(timestamp));

	if (is_ext) {
		char *resp;
		if (rc == 0)
			resp = build_result(id, lat, lon, acc, alt, timestamp);
		else
			resp = build_error(id, strerror(-rc));

		if (resp) {
			printf("%s\n", resp);
			fflush(stdout);
			free(resp);
		}
	} else {
		if (rc == 0) {
			print_plain_result(lat, lon, acc, alt, timestamp);
		} else {
			fprintf(stderr, "Error: %s\n", strerror(-rc));
#if defined(__APPLE__)
			if (rc == -EPERM || rc == -ETIMEDOUT)
				fprintf(stderr,
					"Allow location access in:\n"
					"  System Settings > Privacy & Security\n"
					"  > Location Services > locate\n");
#endif
		}
	}

	return rc == 0 ? 0 : 1;
}
