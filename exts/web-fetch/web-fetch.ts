import { Effect } from "effect"
import { HttpClient, HttpClientRequest } from "@effect/platform"
import { NodeContext, NodeHttpClient, NodeRuntime } from "@effect/platform-node"
import * as readline from "node:readline"
import * as fs from "node:fs"
import TurndownService from "turndown"

type Format = "markdown" | "text" | "html"

interface FetchParams {
	url: string
	format?: Format
	timeout?: number
	render_js?: boolean
	js_wait_ms?: number
}

interface FetchResult {
	content: string
	url: string
	content_type: string
	is_image: boolean
}

const MAX_RESPONSE_SIZE = 5 * 1024 * 1024
const DEFAULT_TIMEOUT = 30_000
const MAX_TIMEOUT = 120_000
const DEFAULT_JS_WAIT_MS = 2000
const MAX_JS_WAIT_MS = 10_000

const BROWSER_UA =
	"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36"

const IMAGE_MIME_PREFIXES = [
	"image/png",
	"image/jpeg",
	"image/gif",
	"image/webp",
	"image/svg+xml",
	"image/avif",
	"image/apng",
	"image/bmp",
	"image/x-icon",
	"image/tiff",
]

function isImageMime(mime: string): boolean {
	return IMAGE_MIME_PREFIXES.some((p) => mime.startsWith(p))
}

function buildAcceptHeader(format: Format): string {
	switch (format) {
		case "markdown":
			return "text/markdown;q=1.0, text/x-markdown;q=0.9, text/plain;q=0.8, text/html;q=0.7, */*;q=0.1"
		case "text":
			return "text/plain;q=1.0, text/markdown;q=0.9, text/html;q=0.8, */*;q=0.1"
		case "html":
			return "text/html;q=1.0, application/xhtml+xml;q=0.9, text/plain;q=0.8, text/markdown;q=0.7, */*;q=0.1"
	}
}

function extractTextFromHTML(html: string): string {
	let text = ""
	let skipDepth = 0
	const skipTags = new Set(["script", "style", "noscript", "iframe", "object", "embed"])
	const tagRe = /<(\/?)(\w+)[^>]*>/g
	let lastIndex = 0
	let match: RegExpExecArray | null
	while ((match = tagRe.exec(html)) !== null) {
		if (skipDepth === 0) {
			text += html.substring(lastIndex, match.index)
		}
		const tagName = match[2].toLowerCase()
		if (skipTags.has(tagName)) {
			if (match[1] === "/") {
				if (skipDepth > 0) skipDepth--
			} else {
				skipDepth++
			}
		}
		lastIndex = match.index + match[0].length
	}
	if (skipDepth === 0) {
		text += html.substring(lastIndex)
	}
	return text.trim()
}

const turndown = new TurndownService({
	headingStyle: "atx",
	hr: "---",
	bulletListMarker: "-",
	codeBlockStyle: "fenced",
	emDelimiter: "*",
})
turndown.remove(["script", "style", "meta", "link"])

function htmlToMarkdown(html: string): string {
	return turndown.turndown(html)
}

function checkSizeLimit(contentLength: string | undefined, byteLength: number): Effect.Effect<void, Error> {
	return Effect.gen(function* () {
		if (contentLength && parseInt(contentLength) > MAX_RESPONSE_SIZE) {
			yield* Effect.fail(new Error("Response too large (exceeds 5MB limit)"))
		}
		if (byteLength > MAX_RESPONSE_SIZE) {
			yield* Effect.fail(new Error("Response too large (exceeds 5MB limit)"))
		}
	})
}

function convertContent(content: string, format: Format, contentType: string): string {
	switch (format) {
		case "markdown":
			return contentType.includes("text/html") ? htmlToMarkdown(content) : content
		case "text":
			return contentType.includes("text/html") ? extractTextFromHTML(content) : content
		case "html":
			return content
	}
}

function makeRequest(url: string, format: Format, client: HttpClient.HttpClient) {
	const request = HttpClientRequest.get(url).pipe(
		HttpClientRequest.setHeaders({
			"User-Agent": BROWSER_UA,
			Accept: buildAcceptHeader(format),
			"Accept-Language": "en-US,en;q=0.9",
		}),
	)
	const httpOk = HttpClient.filterStatusOk(client)
	return httpOk.execute(request).pipe(
		Effect.catchIf(
			(err) =>
				err._tag === "ResponseError" &&
				err.reason === "StatusCode" &&
				err.response.status === 403,
			() =>
				httpOk.execute(
					HttpClientRequest.get(url).pipe(
						HttpClientRequest.setHeaders({
							"User-Agent": "web_fetch",
							Accept: buildAcceptHeader(format),
						}),
					),
				),
		),
	)
}

function webFetch(params: FetchParams): Effect.Effect<FetchResult, Error, HttpClient.HttpClient> {
	return Effect.gen(function* () {
		if (!params.url.startsWith("http://") && !params.url.startsWith("https://")) {
			yield* Effect.fail(new Error("URL must start with http:// or https://"))
		}

		const format: Format = params.format ?? "markdown"
		const timeoutMs = Math.min((params.timeout ?? DEFAULT_TIMEOUT / 1000) * 1000, MAX_TIMEOUT)
		const client = yield* HttpClient.HttpClient

		const response = yield* makeRequest(params.url, format, client).pipe(
			Effect.timeoutFail({ duration: timeoutMs, onTimeout: () => new Error("Request timed out") }),
			Effect.catchTag("ResponseError", (err) =>
				Effect.fail(new Error(`HTTP ${err.response.status}: ${err.reason}`)),
			),
		)

		const contentLength = response.headers["content-length"]
		const contentType: string = response.headers["content-type"] ?? ""
		const mime = contentType.split(";")[0]?.trim().toLowerCase() ?? ""

		const arrayBuffer = yield* response.arrayBuffer
		yield* checkSizeLimit(contentLength, arrayBuffer.byteLength)

		if (isImageMime(mime)) {
			const base64 = Buffer.from(arrayBuffer).toString("base64")
			return {
				content: `data:${mime};base64,${base64}`,
				url: params.url,
				content_type: contentType,
				is_image: true,
			}
		}

		const body = new TextDecoder().decode(arrayBuffer)
		const output = convertContent(body, format, contentType)

		return { content: output, url: params.url, content_type: contentType, is_image: false }
	})
}

const CHROMIUM_CANDIDATES = [
	"/snap/chromium/current/usr/lib/chromium-browser/chrome",
	"/snap/bin/chromium",
	"/usr/bin/chromium-browser",
	"/usr/bin/chromium",
	"/usr/bin/google-chrome",
	"/usr/bin/google-chrome-stable",
]

function findChromium(): string | null {
	const envPath = process.env.CHROMIUM_PATH
	if (envPath) {
		try {
			fs.accessSync(envPath, fs.constants.X_OK)
			return envPath
		} catch { /* fall through */ }
	}
	for (const p of CHROMIUM_CANDIDATES) {
		try {
			fs.accessSync(p, fs.constants.X_OK)
			return p
		} catch { /* try next */ }
	}
	return null
}

function fetchWithJs(params: FetchParams): Effect.Effect<FetchResult, Error> {
	return Effect.gen(function* () {
		if (!params.url.startsWith("http://") && !params.url.startsWith("https://")) {
			yield* Effect.fail(new Error("URL must start with http:// or https://"))
		}

		const format: Format = params.format ?? "markdown"
		const jsWait = Math.min(params.js_wait_ms ?? DEFAULT_JS_WAIT_MS, MAX_JS_WAIT_MS)
		const timeoutMs = Math.min((params.timeout ?? DEFAULT_TIMEOUT / 1000) * 1000, MAX_TIMEOUT)

		const playwrightModule = yield* Effect.tryPromise({
			try: () => import("playwright-core"),
			catch: (e) => new Error(`playwright-core not available: ${e}`),
		})

		const execPath = findChromium()
			?? (() => {
				try {
					return playwrightModule.chromium.executablePath()
				} catch { return null }
			})()

		if (!execPath) {
			yield* Effect.fail(
				new Error(
					"No Chromium found. Install chromium or set CHROMIUM_PATH env var, "
					+ "or run: npx playwright install chromium",
				),
			)
		}

		const browser = yield* Effect.tryPromise({
			try: () =>
				playwrightModule.chromium.launchPersistentContext(
					`/tmp/chromium-webfetch-${process.pid}`,
					{
						executablePath: execPath,
						headless: true,
						args: [
							"--no-sandbox",
							"--disable-setuid-sandbox",
							"--disable-gpu",
							"--disable-dev-shm-usage",
							"--disable-extensions",
							"--disable-background-networking",
							"--disable-sync",
							"--no-first-run",
							"--disable-default-apps",
							"--metrics-recording-only",
						],
						chromiumSandbox: false,
					},
				),
			catch: (e) => new Error(`Failed to launch browser: ${e}`),
		})

		const page = yield* Effect.tryPromise({
			try: () => browser.newPage(),
			catch: (e) => new Error(`Failed to create page: ${e}`),
		})

		yield* Effect.tryPromise({
			try: () =>
				page.goto(params.url, {
					waitUntil: "networkidle",
					timeout: timeoutMs,
				}),
			catch: (e) => new Error(`Navigation failed: ${e}`),
		})

		if (jsWait > 0) {
			yield* Effect.tryPromise({
				try: () => page.waitForTimeout(jsWait),
				catch: () => {},
			})
		}

		const html = yield* Effect.tryPromise({
			try: () => page.content(),
			catch: (e) => new Error(`Failed to get page content: ${e}`),
		})

		const finalUrl = page.url()

		yield* Effect.tryPromise({
			try: () => browser.close(),
			catch: () => {},
		})

		const output = convertContent(html, format, "text/html")

		return { content: output, url: finalUrl, content_type: "text/html", is_image: false }
	})
}

function sendResponse(id: number, result: unknown): void {
	process.stdout.write(JSON.stringify({ jsonrpc: "2.0", id, result }) + "\n")
}

function sendError(id: number, code: number, message: string): void {
	process.stdout.write(JSON.stringify({ jsonrpc: "2.0", id, error: { code, message } }) + "\n")
}

function handleRequest(line: string): void {
	let req: { id?: number; method?: string; params?: FetchParams }
	try {
		req = JSON.parse(line)
	} catch {
		sendError(0, -32700, "Parse error")
		return
	}

	const id = req.id ?? 1

	if (req.method !== "run" || !req.params) {
		sendError(id, -32600, "Invalid request: method must be 'run' with params")
		return
	}

	if (!req.params.url) {
		sendError(id, -32602, "Missing required param: url")
		return
	}

	if (req.params.render_js) {
		fetchWithJs(req.params).pipe(
			Effect.match({
				onSuccess: (result) => sendResponse(id, result),
				onFailure: (err) => sendError(id, -32603, err instanceof Error ? err.message : String(err)),
			}),
			NodeRuntime.runMain,
		)
	} else {
		webFetch(req.params).pipe(
			Effect.provide(NodeHttpClient.layer),
			Effect.provide(NodeContext.layer),
			Effect.match({
				onSuccess: (result) => sendResponse(id, result),
				onFailure: (err) => sendError(id, -32603, err instanceof Error ? err.message : String(err)),
			}),
			NodeRuntime.runMain,
		)
	}
}

const rl = readline.createInterface({ input: process.stdin })
rl.on("line", (line) => {
	rl.close()
	handleRequest(line.trim())
})
