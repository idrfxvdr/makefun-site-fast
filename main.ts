// main.ts — the harness driver, zero-copy C/WASM fast path.
//
// The harness runs `node /bot/main.ts` (node22): it reads every case from /input/cases.json,
// answers each with the freestanding C program compiled to wasm32+SIMD128 (src/sitegen.c ->
// sitegen.wasm), and writes /output/answers.json. The whole build runs on raw UTF-8 bytes inside
// wasm: cases.json is read as a Buffer and copied ONCE into linear memory, mg_run parses the JSON
// in-C, runs the entire makefun-site pipeline over bytes, builds the whole answers.json as bytes
// in-C (including JSON escaping), and main.ts bulk-copies the answer out and writeFileSync's it.
// No JS strings, no JSON.parse, no JSON.stringify on the hot path.
//
// V8's incremental marking is turned off before the wasm is touched, as the house baseline does.
// solve.js still exports a correct run() (the differential oracle); the graded path is the wasm.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

setFlagsFromString('--no-incremental-marking');

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

// The wasm module, resolved next to this file. mg_run(inPtr, inLen) reads cases.json bytes at
// inPtr and writes {out_ptr, out_len} into the control struct at address 0.
const WASM_URL = new URL('./sitegen.wasm', import.meta.url);
const wasmBytes = readFileSync(WASM_URL);

const INPUT_BASE = 1 * 1024 * 1024; // 1 MiB: cases.json copied here; arena starts above (4 MiB+)
const ARENA_BASE = 4 * 1024 * 1024;

const { instance } = await WebAssembly.instantiate(wasmBytes, {});
const ex = instance.exports as {
  memory: WebAssembly.Memory;
  mg_run: (inPtr: number, inLen: number) => number;
};

// Read cases.json as a RAW Buffer (no JSON.parse).
const input = readFileSync(join(INPUT_DIR, 'cases.json'));

// Ensure linear memory holds the input at INPUT_BASE plus room for the arena to grow.
// 1 MiB initial + grow to cover input + a generous margin for the 7.4 MB answer and the build.
const needed = INPUT_BASE + input.byteLength + 64 * 1024 * 1024;
const cur = ex.memory.buffer.byteLength;
if (needed > cur) {
  const pages = Math.ceil((needed - cur) / 65536);
  ex.memory.grow(pages);
}

// One bulk copy in.
new Uint8Array(ex.memory.buffer, INPUT_BASE, input.byteLength).set(input);

// Run the whole build in C. mg_run parses cases.json, builds answers.json as bytes in linear
// memory, and writes {out_ptr, out_len} into the control struct at address 0.
ex.mg_run(INPUT_BASE, input.byteLength);

// Read {out_ptr, out_len} from the control struct at address 0 (no memory growth happens after
// this point, so the buffer view stays valid for the bulk copy out).
const ctrl = new Uint32Array(ex.memory.buffer, 0, 3);
const outPtr = ctrl[0];
const outLen = ctrl[1];

// One bulk copy out, straight to disk as a Buffer (no JS string is ever materialised).
const outBuf = Buffer.from(ex.memory.buffer, outPtr, outLen);
writeFileSync(join(OUTPUT_DIR, 'answers.json'), outBuf);