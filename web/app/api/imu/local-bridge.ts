const bridgeOrigin = "http://127.0.0.1:8765";

export function isLocal(request: Request): boolean {
  const url = new URL(request.url);
  return ["127.0.0.1", "localhost", "[::1]"].includes(url.hostname);
}

export async function bridgeRequest(path: string, request: Request, body?: string) {
  const controller = new AbortController();
  const onAbort = () => controller.abort();
  request.signal.addEventListener("abort", onAbort, { once: true });
  const timeout = setTimeout(() => controller.abort(), 2500);
  try {
    return await fetch(`${bridgeOrigin}${path}`, {
      method: body === undefined ? "GET" : "POST",
      headers: { "X-IMU-Bridge": "nu40-local-v1", "Content-Type": "application/json" },
      body,
      cache: "no-store",
      signal: controller.signal,
    });
  } catch {
    request.signal.removeEventListener("abort", onAbort);
    return null;
  } finally {
    clearTimeout(timeout);
  }
}

export function unavailable() {
  return Response.json({ error: "USB 연결 서비스가 꺼져 있습니다. 로컬 IMU 서비스를 실행해 주세요." }, { status: 503 });
}
