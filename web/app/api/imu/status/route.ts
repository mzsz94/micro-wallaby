import { bridgeRequest, isLocal, unavailable } from "../local-bridge";

export async function GET(request: Request) {
  if (!isLocal(request)) return new Response("Local dashboard only", { status: 403 });
  const upstream = await bridgeRequest("/status", request);
  if (!upstream?.ok) return unavailable();
  return new Response(upstream.body, { headers: { "Content-Type": "application/json", "Cache-Control": "no-store" } });
}
