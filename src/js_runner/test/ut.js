async function run(args) {
	const out = args.out || "/tmp/morph-js-runner-ut";
	const cases = [];

	function assert(cond, message) {
		if (!cond)
			throw new Error(message);
	}

	async function test(name, fn) {
		await fn();
		cases.push(name);
	}

	const textPath = out + "/fs.txt";
	const binPath = out + "/fs.bin";
	const canvasPng = out + "/canvas.png";
	const canvasJpg = out + "/canvas.jpg";
	const basePng = out + "/base.png";
	const overlayPng = out + "/overlay.png";
	const resizedPng = out + "/resized.png";
	const cropPng = out + "/crop.png";
	const extendPng = out + "/extend.png";
	const rotatePng = out + "/rotate.png";
	const composePng = out + "/compose.png";
	const convertJpg = out + "/convert.jpg";
	const frameJpg = out + "/frame.jpg";
	const chainPng = out + "/chain.png";
	const readbackPng = out + "/readback.png";

	await test("morph.fs.writeText/readText", async function () {
		morph.fs.writeText(textPath, "hello morph");
		assert(morph.fs.readText(textPath) === "hello morph",
			"readText mismatch");
	});

	await test("morph.fs.writeFile/readFile", async function () {
		const bytes = new Uint8Array([1, 2, 3, 4, 255]);
		morph.fs.writeFile(binPath, bytes);
		const read = new Uint8Array(morph.fs.readFile(binPath));
		assert(read.length === 5 && read[4] === 255,
			"readFile mismatch");
	});

	await test("morph.env.get", async function () {
		assert(morph.env.get("MORPH_JS_RUNNER_UT") === "ok",
			"env mismatch");
	});

	await test("morph.exec", async function () {
		const result = morph.exec("printf morph-exec");
		assert(String(result).indexOf("morph-exec") >= 0,
			"exec mismatch");
	});

	await test("morph.canvas.create/getContext/drawing/toFile", async function () {
		const canvas = morph.canvas.create({ width: 80, height: 48 });
		assert(canvas.width === 80 && canvas.height === 48,
			"canvas dimensions mismatch");
		const ctx = canvas.getContext("2d");
		ctx.fillStyle = "#ffffff";
		ctx.fillRect(0, 0, 80, 48);
		ctx.strokeStyle = "#111111";
		ctx.strokeRect(2, 2, 76, 44);
		ctx.beginPath();
		ctx.moveTo(8, 40);
		ctx.lineTo(72, 8);
		ctx.stroke();
		ctx.save();
		ctx.translate(10, 10);
		ctx.fillStyle = "#4477cc";
		ctx.rect(0, 0, 20, 10);
		ctx.fill();
		ctx.restore();
		ctx.beginPath();
		ctx.arc(62, 34, 7, 0, Math.PI * 2);
		ctx.fillStyle = "#cc3344";
		ctx.fill();
		ctx.fillStyle = "#111111";
		ctx.fillText("ut", 8, 24);
		ctx.strokeText("x", 54, 22);
		morph.canvas.toFile({ canvas, output: canvasPng });
		canvas.toFile(canvasJpg);
	});

	await test("morph.canvas.toBuffer/loadImage/drawImage", async function () {
		const image = morph.canvas.loadImage({ input: canvasPng });
		assert(image.width === 80 && image.height === 48,
			"loadImage dimensions mismatch");
		const canvas = morph.canvas.create({ width: 96, height: 64 });
		const ctx = canvas.getContext("2d");
		ctx.fillStyle = "#222222";
		ctx.fillRect(0, 0, 96, 64);
		ctx.drawImage(image, 8, 8, 80, 48);
		const buf = morph.canvas.toBuffer({ canvas });
		assert(buf.byteLength > 0, "canvas toBuffer empty");
		canvas.toFile(basePng);
	});

	await test("morph.canvas.loadImage/drawImage jpeg", async function () {
		const image = morph.canvas.loadImage({ input: canvasJpg });
		assert(image.width === 80 && image.height === 48,
			"jpeg loadImage dimensions mismatch");
		const canvas = morph.canvas.create({ width: 96, height: 64 });
		const ctx = canvas.getContext("2d");
		ctx.fillStyle = "#ffffff";
		ctx.fillRect(0, 0, 96, 64);
		ctx.drawImage(image, 8, 8, 80, 48);
		assert(morph.canvas.toBuffer({ canvas }).byteLength > 0,
			"jpeg drawImage output empty");
	});

	await test("morph.image.metadata", async function () {
		const meta = await morph.image.metadata({ input: basePng });
		assert(meta.width === 96 && meta.height === 64,
			"metadata mismatch");
	});

	await test("morph.image.resize", async function () {
		const meta = await morph.image.resize({
			input: basePng,
			output: resizedPng,
			width: 48,
			height: 32
		});
		assert(meta.width === 48 && meta.height === 32,
			"resize mismatch");
	});

	await test("morph.image.crop/extract", async function () {
		const meta = await morph.image.crop({
			input: basePng,
			output: cropPng,
			left: 4,
			top: 4,
			width: 32,
			height: 24
		});
		assert(meta.width === 32 && meta.height === 24,
			"crop mismatch");
		const meta2 = await morph.image.extract({
			input: basePng,
			output: out + "/extract.png",
			left: 0,
			top: 0,
			width: 16,
			height: 16
		});
		assert(meta2.width === 16 && meta2.height === 16,
			"extract mismatch");
	});

	await test("morph.image.extend", async function () {
		const meta = await morph.image.extend({
			input: cropPng,
			output: extendPng,
			top: 3,
			bottom: 5,
			left: 7,
			right: 9,
			background: { r: 255, g: 255, b: 255, alpha: 1 }
		});
		assert(meta.width === 48 && meta.height === 32,
			"extend mismatch");
	});

	await test("morph.image.rotate", async function () {
		const meta = await morph.image.rotate({
			input: cropPng,
			output: rotatePng,
			angle: -3
		});
		assert(meta.width >= 32 && meta.height >= 24,
			"rotate mismatch");
	});

	await test("morph.image.compose", async function () {
		const overlay = morph.canvas.create({ width: 16, height: 16 });
		const ctx = overlay.getContext("2d");
		ctx.fillStyle = "#ff0000";
		ctx.fillRect(0, 0, 16, 16);
		overlay.toFile(overlayPng);
		const meta = await morph.image.compose({
			input: basePng,
			output: composePng,
			overlays: [{ input: overlayPng, left: 12, top: 10 }]
		});
		assert(meta.width === 96 && meta.height === 64,
			"compose mismatch");
	});

	await test("morph.image.convert", async function () {
		const meta = await morph.image.convert({
			input: composePng,
			output: convertJpg
		});
		assert(meta.width === 96 && meta.height === 64,
			"convert mismatch");
	});

	await test("morph.image.frame", async function () {
		const meta = await morph.image.frame({
			input: composePng,
			output: frameJpg,
			style: "neon",
			caption: "runner ut",
			padding: 16
		});
		assert(meta.width === 128 && meta.height === 112,
			"frame mismatch");
	});

	await test("morph.image.open handle methods", async function () {
		const img = morph.image.open(basePng);
		const meta = await img.metadata();
		assert(meta.width === 96 && meta.height === 64,
			"open metadata mismatch");
		await img.resize(64, 42)
			.extract({ left: 0, top: 0, width: 40, height: 30 })
			.extend({
				top: 2,
				bottom: 2,
				left: 2,
				right: 2,
				background: "#ffffff"
			})
			.rotate(2)
			.blur(0.2)
			.sharpen()
			.grayscale()
			.greyscale()
			.flatten()
			.png()
			.toFile(chainPng);
		const chainMeta = await morph.image.metadata({ input: chainPng });
		assert(chainMeta.width > 0 && chainMeta.height > 0,
			"chain output missing");
	});

	await test("morph.image.create/toBuffer/open buffer/jpeg/webp", async function () {
		const img = morph.image.create({
			width: 20,
			height: 12,
			channels: 4,
			background: { r: 20, g: 80, b: 160, alpha: 1 }
		});
		const png = await img.png().toBuffer();
		assert(png.byteLength > 0, "png buffer empty");
		await morph.image.open(png).toFile(readbackPng);
		const jpg = await morph.image.open(readbackPng).jpeg().toBuffer();
		assert(jpg.byteLength > 0, "jpeg buffer empty");
		if (morph.env.get("ANDROID_ROOT")) {
			cases.push("webp buffer skipped on android");
		} else {
			const webp = await morph.image.open(readbackPng).webp().toBuffer();
			assert(webp.byteLength > 0, "webp buffer empty");
		}
	});

	await test("WebAssembly.Module/Instance/instantiate/compile/Memory",
		async function () {
			const bytes = new Uint8Array([
				0, 97, 115, 109, 1, 0, 0, 0, 1, 7, 1, 96, 2, 127,
				127, 1, 127, 3, 2, 1, 0, 7, 7, 1, 3, 97, 100,
				100, 0, 0, 10, 9, 1, 7, 0, 32, 0, 32, 1, 106, 11
			]);
			const compiled = await WebAssembly.compile(bytes.buffer);
			const instance = new WebAssembly.Instance(compiled, {});
			assert(instance.exports.add(20, 22) === 42,
				"wasm instance mismatch");
			const result = await WebAssembly.instantiate(bytes.buffer, {});
			assert(result.instance.exports.add(1, 2) === 3,
				"wasm instantiate mismatch");
			const memory = new WebAssembly.Memory({ initial: 1 });
			assert(memory.buffer.byteLength === 65536,
				"wasm memory mismatch");
		});

	await test("require disabled", async function () {
		let failed = false;
		try {
			require("sharp");
		} catch (e) {
			failed = String(e).indexOf("require() is disabled") >= 0;
		}
		assert(failed, "require should be disabled");
	});

	if (args.runNetwork) {
		await test("morph.fetch", async function () {
			const response = await morph.fetch(args.url ||
				"https://example.com");
			const text = await response.text();
			assert(response.status >= 200 && text.length > 0,
				"fetch mismatch");
		});
	} else {
		cases.push("morph.fetch skipped");
	}

	return {
		ok: true,
		count: cases.length,
		cases
	};
}
