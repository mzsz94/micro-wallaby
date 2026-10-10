import { bridgeRequest, isLocal, unavailable } from "../local-bridge";

export async function GET(request: Request) {
  if (!isLocal(request)) return new Response("Local dashboard only", { status: 403 });
  const upstream = await bridgeRequest("/events", request);
  if (!upstream?.ok || !upstream.body) return unavailable();
  return new Response(upstream.body, {
    headers: {
      "Content-Type": "text/event-stream; charset=utf-8",
      "Cache-Control": "no-store, no-transform",
      "X-Accel-Buffering": "no",
    },
  });
}
