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

test("server-renders the NU Play daily game", async () => {
  const response = await render();
  assert.equal(response.status, 200);
  assert.match(response.headers.get("content-type") ?? "", /^text\/html\b/i);

  const html = await response.text();
  assert.match(html, /<title>NU Play Lab<\/title>/i);
  assert.match(html, /오늘의 성장 놀이/);
  assert.match(html, /Bluetooth 연결/);
  assert.match(html, /별빛 우체국/);
  assert.match(html, /노란색 별 카드를 찾아주세요/);
  assert.match(html, /게임 초안 만들기/);
  assert.match(html, /부모님 최종 확인/);
  assert.match(html, /승인하고 시작/);
  assert.match(html, /아이 나이/);
  assert.match(html, /오늘의 공간/);
  assert.match(html, /센서 리모컨/);
  assert.doesNotMatch(html, /codex-preview|SkeletonPreview|Starter Project/);
});

test("keeps firmware and web BLE identity in sync", async () => {
  const [component, firmware, config] = await Promise.all([
    readFile(new URL("../app/DailyGameLab.tsx", import.meta.url), "utf8"),
    readFile(new URL("../../src/main.c", import.meta.url), "utf8"),
    readFile(new URL("../../prj.conf", import.meta.url), "utf8"),
  ]);

  assert.match(component, /MZSZ-TOY/);
  assert.match(config, /CONFIG_BT_DEVICE_NAME="MZSZ-TOY"/);
  assert.match(component, /7b1e0001-21a8-4f8c-bb2d-4c7d2156f001/);
  assert.match(component, /7b1e0002-21a8-4f8c-bb2d-4c7d2156f001/);
  assert.match(component, /7b1e0003-21a8-4f8c-bb2d-4c7d2156f001/);
  assert.match(firmware, /0x7b1e0001/);
  assert.match(firmware, /0x7b1e0002/);
  assert.match(firmware, /0x7b1e0003/);
  assert.match(firmware, /sizeof\(struct sensor_packet\) == 20/);
  assert.match(component, /"조이스틱"/);
  assert.match(component, /value\.getUint8\(18\)/);
});

test("daily game generator is bounded to child-safe mechanics", async () => {
  const component = await readFile(new URL("../app/DailyGameLab.tsx", import.meta.url), "utf8");

  assert.match(component, /type GameKind = "color" \| "count" \| "shape" \| "emotion"/);
  assert.match(component, /createDailyGame/);
  assert.match(component, /nu-play-prompt:/);
  assert.match(component, /nu-play-plan:/);
  assert.match(component, /type ApprovalState = "draft" \| "approved"/);
  assert.doesNotMatch(component, /eval\(|new Function\(/);
});
