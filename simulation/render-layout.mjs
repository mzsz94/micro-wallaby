/** An iframe may report a zero canvas size before its first layout. */
export function arenaLayout(width, height, world) {
  if (![width, height, world.width, world.height].every(Number.isFinite) ||
      width <= 56 || height <= 65 || world.width <= 0 || world.height <= 0) return null;
  const scale = Math.min((width - 56) / world.width, (height - 65) / world.height);
  return { scale, offsetX: (width - world.width * scale) / 2,
    offsetY: (height - world.height * scale) / 2 - 8 };
}
