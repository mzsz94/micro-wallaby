import assert from "node:assert/strict";
import { readFile } from "node:fs/promises";
import test from "node:test";

async function render() {
  const workerUrl = new URL("../dist/server/index.js", import.meta.url);
  workerUrl.searchParams.set("test", `${process.pid}-${Date.now()}`);
  const { default: worker } = await import(workerUrl.href);

  return worker.fetch(
    new Request("http://localhost/", { headers: { accept: "text/html" } }),
    { ASSETS: { fetch: async () => new Response("Not found", { status: 404 }) } },
    { waitUntil() {}, passThroughOnException() {} },
  );
}

test("server-renders the NU Home simulation", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<title>NU Home Lab<\/title>/i);
  assert.match(html, /우리 집 시뮬레이션/);
  assert.match(html, /Bluetooth 연결/);
  assert.match(html, /거실/);
  assert.match(html, /센서 리모컨/);
  assert.doesNotMatch(html, /codex-preview|SkeletonPreview|Starter Project/);
});

test("keeps firmware and web BLE UUIDs in sync", async () => {
  const [component, firmware] = await Promise.all([
    readFile(new URL("../app/HomeLab.tsx", import.meta.url), "utf8"),
    readFile(new URL("../../src/main.c", import.meta.url), "utf8"),
  ]);

  assert.match(component, /7b1e0001-21a8-4f8c-bb2d-4c7d2156f001/);
  assert.match(component, /7b1e0002-21a8-4f8c-bb2d-4c7d2156f001/);
  assert.match(component, /7b1e0003-21a8-4f8c-bb2d-4c7d2156f001/);
  assert.match(firmware, /0x7b1e0001/);
  assert.match(firmware, /0x7b1e0002/);
  assert.match(firmware, /0x7b1e0003/);
  assert.match(firmware, /sizeof\(struct sensor_packet\) == 18/);
});
