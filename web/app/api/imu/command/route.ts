import { bridgeRequest, isLocal, unavailable } from "../local-bridge";

const allowed = new Set(["imu status", "imu retry", "imu stream on", "imu stream off"]);

export async function POST(request: Request) {
  if (!isLocal(request)) return new Response("Local dashboard only", { status: 403 });
  const origin = request.headers.get("origin");
  if (origin && origin !== new URL(request.url).origin) {
    return Response.json({ error: "다른 웹사이트에서 보낸 명령은 허용하지 않습니다." }, { status: 403 });
  }
  let body;
  try {
    const text = await request.text();
    if (text.length > 256) throw new Error("Body too large");
    body = JSON.parse(text);
  } catch {
    return Response.json({ error: "명령 형식이 올바르지 않습니다." }, { status: 400 });
  }
  if (typeof body?.command !== "string" || !allowed.has(body.command)) {
    return Response.json({ error: "지원하지 않는 명령입니다." }, { status: 400 });
  }
  const upstream = await bridgeRequest("/command", request, JSON.stringify({ command: body.command }));
  if (!upstream) return unavailable();
  return new Response(upstream.body, { status: upstream.status, headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
}
