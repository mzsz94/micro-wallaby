import { CONFIG } from './model.mjs';

/** Close view of the model's measured trail, never a pre-drawn demonstration path. */
export function drawDemoArena({ ctx, width, height }, s) {
  if (width < 80 || height < 80) return;
  const points = [...s.trail, s.pose];
  const bounds = s.marker ? [...points, s.marker] : points;
  const minX = Math.min(...bounds.map(p => p.x));
  const maxX = Math.max(...bounds.map(p => p.x));
  const minY = Math.min(...bounds.map(p => p.y));
  const maxY = Math.max(...bounds.map(p => p.y));
  // Short voice scenes travel only a few decimeters: use the full compact map
  // height, with enough world-space padding for the endpoint annotations.
  const spanX = Math.max(.72, maxX - minX + .28);
  const spanY = Math.max(.40, maxY - minY + .24);
  const scale = Math.min((width - 70) / spanX, (height - 74) / spanY);
  const cx = (minX + maxX) / 2, cy = (minY + maxY) / 2;
  const xy = (x, y) => [width / 2 + (x - cx) * scale, height / 2 + 3 - (y - cy) * scale];
  const labelY = y => Math.max(45, Math.min(height - 37, y));
  ctx.save();
  ctx.beginPath(); ctx.rect(18, 34, width - 36, height - 65); ctx.clip();
  ctx.lineWidth = 1; ctx.strokeStyle = '#263b3d';
  for (let x = Math.floor((cx - spanX) * 10) / 10; x < cx + spanX; x += .1) {
    ctx.beginPath(); ctx.moveTo(...xy(x, cy - spanY)); ctx.lineTo(...xy(x, cy + spanY)); ctx.stroke();
  }
  for (let y = Math.floor((cy - spanY) * 10) / 10; y < cy + spanY; y += .1) {
    ctx.beginPath(); ctx.moveTo(...xy(cx - spanX, y)); ctx.lineTo(...xy(cx + spanX, y)); ctx.stroke();
  }
  for (const obstacle of s.world.obstacles) {
    ctx.beginPath(); ctx.arc(...xy(obstacle.x, obstacle.y), obstacle.r * scale, 0, Math.PI * 2);
    ctx.fillStyle = '#405151'; ctx.fill();
  }
  if (s.marker) {
    const p = xy(s.marker.x, s.marker.y);
    ctx.strokeStyle = s.marker.detected ? '#ff766b' : '#a3a897'; ctx.lineWidth = 2;
    if (!s.marker.detected) ctx.setLineDash([3, 3]);
    ctx.beginPath(); ctx.arc(...p, s.marker.detected ? 19 : 11, 0, Math.PI * 2); ctx.stroke(); ctx.setLineDash([]);
    ctx.textAlign = 'center'; ctx.fillStyle = ctx.strokeStyle; ctx.font = 'bold 14px monospace';
    ctx.fillText(s.marker.detected ? '!' : '?', p[0], p[1] + 5);
    const requestedLabelY = p[1] - (s.marker.detected ? 25 : 17);
    const besideMarker = requestedLabelY < 45;
    if (besideMarker) ctx.textAlign = 'right';
    ctx.font = '8px monospace';
    ctx.fillText(s.marker.detected ? s.marker.label : 'MOCK TARGET', p[0] - (besideMarker ? 25 : 0), besideMarker ? p[1] - 10 : labelY(requestedLabelY));
    ctx.textAlign = 'left';
  }
  const start = xy(s.trail[0].x, s.trail[0].y);
  if (s.trail.length > 1) {
    ctx.beginPath(); points.forEach((p, i) => i ? ctx.lineTo(...xy(p.x, p.y)) : ctx.moveTo(...xy(p.x, p.y)));
    ctx.strokeStyle = s.accent + '24'; ctx.lineWidth = 12; ctx.lineCap = 'round'; ctx.lineJoin = 'round'; ctx.stroke();
    ctx.strokeStyle = s.accent; ctx.lineWidth = 3.5; ctx.stroke();
    for (let i = 6; i < s.trail.length; i += 9) {
      ctx.beginPath(); ctx.arc(...xy(s.trail[i].x, s.trail[i].y), 2.5, 0, Math.PI * 2); ctx.fillStyle = '#e1fff1'; ctx.fill();
    }
  }
  ctx.beginPath(); ctx.arc(...start, 5, 0, Math.PI * 2); ctx.fillStyle = '#0d1b20'; ctx.fill();
  ctx.strokeStyle = '#d3e4e3'; ctx.lineWidth = 2; ctx.stroke();
  ctx.font = '9px monospace'; ctx.fillStyle = '#bacdca'; ctx.fillText('START', start[0] - 15, labelY(start[1] + 24));
  const position = xy(s.pose.x, s.pose.y);
  const radius = Math.min(13, CONFIG.radius * scale);
  ctx.save(); ctx.translate(...position); ctx.rotate(-s.pose.theta);
  ctx.beginPath(); ctx.arc(0, 0, radius + 7, -.65, .65); ctx.strokeStyle = s.accent; ctx.lineWidth = 2; ctx.stroke();
  ctx.fillStyle = '#526d66';
  for (const side of [-1, 1]) ctx.fillRect(-radius * .7, side * radius * .88 - 3, radius * 1.4, 6);
  ctx.beginPath(); ctx.arc(0, 0, radius, 0, Math.PI * 2); ctx.fillStyle = s.accent; ctx.fill();
  ctx.beginPath(); ctx.moveTo(radius * .65, 0); ctx.lineTo(-radius * .3, -radius * .45); ctx.lineTo(-radius * .3, radius * .45); ctx.closePath();
  ctx.fillStyle = '#15352d'; ctx.fill(); ctx.restore();
  ctx.restore();
  ctx.fillStyle = s.accent; ctx.font = 'bold 10px monospace'; ctx.fillText('LIVE TRAJECTORY', 20, 21);
  ctx.textAlign = 'right'; ctx.fillStyle = '#9ab5b1'; ctx.font = '9px monospace'; ctx.fillText('0.1 m / grid · AUTO ZOOM', width - 20, 21);
  ctx.textAlign = 'left'; ctx.font = '10px monospace'; ctx.fillStyle = '#cce4da';
  ctx.fillText(`${s.distanceTraveled.toFixed(2)} m traveled`, 20, height - 13);
  ctx.textAlign = 'right'; ctx.fillStyle = s.accent;
  ctx.fillText(`${Math.abs(s.velocity.linear) < .001 && Math.abs(s.velocity.angular) < .001 ? 'STOP' : 'MOVING'}  ${(s.pose.theta * 180 / Math.PI).toFixed(0)}°`, width - 20, height - 13);
  ctx.textAlign = 'left';
}

/** Expressive concept body; drive and joint animation remain tied to model telemetry. */
export function drawDemoRobot({ ctx, width, height }, s) {
  if (width < 60 || height < 60) return;
  const unit = Math.min(width / 3.1, height / 3.18);
  const locked = s.alertActive || s.motionLocked || ['FAULT', 'DOCK_FAILED'].includes(s.state);
  const phase = locked ? 0 : s.time;
  const moving = Math.abs(s.velocity.linear) + Math.abs(s.velocity.angular) > .005;
  const bob = moving && !locked ? Math.sin((s.wheelAngles.left + s.wheelAngles.right) * 2) * .018 : 0;
  const yaw = (s.joints.headYaw + s.joints.waistYaw * .6) * Math.PI / 180;
  const pitch = s.joints.headPitch * Math.PI / 180;
  const palette = { idle: '#a7e9c6', listening: '#6de3ef', thinking: '#c2adff', happy: '#bcf27e', uncertain: '#ffcb6b', alert: '#ff766b' };
  const color = palette[s.expression] || palette.idle;
  const round = (x, y, w, h, r, fill, stroke) => {
    ctx.beginPath(); ctx.roundRect(x, y, w, h, r); ctx.fillStyle = fill; ctx.fill();
    if (stroke) { ctx.strokeStyle = stroke; ctx.lineWidth = .018; ctx.stroke(); }
  };
  ctx.save(); ctx.translate(width / 2, height * .78); ctx.scale(unit, unit);
  ctx.beginPath(); ctx.ellipse(0, .08, .9, .13, 0, 0, Math.PI * 2); ctx.fillStyle = '#050c1088'; ctx.fill();
  // Large visible tread and spokes rotate with the simulated wheel angles.
  for (const side of [-1, 1]) {
    ctx.save(); ctx.translate(side * .62, -.13);
    round(-.16, -.27, .32, .49, .12, '#263c3f', '#718d83');
    const angle = side < 0 ? s.wheelAngles.left : s.wheelAngles.right;
    ctx.save(); ctx.scale(.72, 1); ctx.rotate(angle);
    for (let i = 0; i < 6; i++) {
      ctx.rotate(Math.PI / 3); ctx.beginPath(); ctx.moveTo(.06, 0); ctx.lineTo(.20, 0);
      ctx.strokeStyle = color; ctx.lineWidth = .045; ctx.stroke();
    }
    ctx.restore(); ctx.beginPath(); ctx.arc(0, 0, .05, 0, Math.PI * 2); ctx.fillStyle = '#9db3a7'; ctx.fill(); ctx.restore();
  }
  ctx.save(); ctx.translate(Math.sin(s.joints.waistYaw * Math.PI / 180) * .13, bob);
  ctx.rotate(s.joints.waistYaw * Math.PI / 180 * .08);
  round(-.51, -.68, 1.02, .63, .12, '#9dad91', '#d0dcc6');
  round(-.38, -.56, .76, .32, .05, '#7e947c');
  round(-.27, -.48, .54, .05, .02, color);
  for (const x of [-.13, 0, .13]) round(x - .025, -.36, .05, .055, .01, '#c4d2b7');
  round(-.105, -.91, .21, .25, .04, '#546e60', '#a2b299');
  ctx.translate(Math.sin(yaw) * .21, -.91 + Math.sin(pitch) * .12);
  ctx.rotate(-yaw * .10);
  // Ear silhouette conveys state; safety stop freezes decorative motion too.
  const earSpread = { idle: .04, listening: -.08, thinking: .11, happy: .22, uncertain: .24, alert: -.13 }[s.expression] || 0;
  for (const side of [-1, 1]) {
    ctx.save(); ctx.translate(side * .48, -.77);
    const asymmetry = s.expression === 'uncertain' && side < 0 ? .30 : 0;
    ctx.rotate(side * (earSpread + asymmetry + (locked ? 0 : Math.sin(phase * 3 + side) * .025)));
    round(-.115, -.61, .23, .70, .095, '#bfd0b4', '#d8e3cd');
    round(-.055, -.50, .11, .46, .04, s.expression === 'alert' ? '#d6917f' : '#829b83');
    ctx.restore();
  }
  ctx.scale(1 - Math.abs(Math.sin(yaw)) * .16, 1 - Math.abs(Math.sin(pitch)) * .1);
  round(-.73, -.83, 1.46, .88, .17, '#c7d7b9', '#e0ead4');
  round(-.65, -.74, 1.30, .66, .12, '#142f32', '#496b5e');
  ctx.save(); ctx.beginPath(); ctx.roundRect(-.65, -.74, 1.30, .66, .12); ctx.clip();
  ctx.strokeStyle = '#4f786126'; ctx.lineWidth = .008;
  for (let y = -.7; y < -.1; y += .06) { ctx.beginPath(); ctx.moveTo(-.65, y); ctx.lineTo(.65, y); ctx.stroke(); }
  const gaze = locked ? 0 : s.expression === 'thinking' ? Math.sin(phase * 1.7) * .055 : Math.sin(phase * .8) * .025;
  const blink = !locked && s.expression === 'idle' && phase % 4.5 > 4.3;
  const line = (points, lineWidth = .043) => {
    ctx.beginPath(); points.forEach((p, i) => i ? ctx.lineTo(...p) : ctx.moveTo(...p));
    ctx.lineWidth = lineWidth; ctx.strokeStyle = color; ctx.lineCap = 'round'; ctx.lineJoin = 'round'; ctx.stroke();
  };
  for (const side of [-1, 1]) {
    const x = side * .30 + gaze;
    if (s.expression === 'happy') line([[x - .11, -.43], [x, -.54], [x + .11, -.43]], .052);
    else if (s.expression === 'alert') {
      round(x - .08, -.54, .16, .17, .028, color);
      line([[x - .13, -.64 + side * .05], [x + .13, -.64 - side * .05]], .045);
    } else if (s.expression === 'uncertain') {
      round(x - .075, side < 0 ? -.53 : -.58, .15, side < 0 ? .11 : .21, .035, color);
      line([[x - .12, -.66], [x + .12, side < 0 ? -.62 : -.71]], .025);
    } else if (s.expression === 'thinking') round(x - .085, -.56, .17, .10, .028, color);
    else round(x - .085, -.58, .17, blink ? .035 : s.expression === 'listening' ? .25 : .20, .045, color);
  }
  if (s.expression === 'happy') line([[-.13, -.27], [0, -.21], [.13, -.27]], .032);
  else if (s.expression === 'uncertain') line([[-.13, -.23], [-.045, -.26], [.045, -.22], [.13, -.25]], .028);
  else if (s.expression === 'alert') line([[-.12, -.25], [.12, -.25]], .033);
  else if (s.expression === 'listening') {
    for (let i = -2; i <= 2; i++) {
      const h = .025 + Math.abs(Math.sin(phase * 7 + i)) * .055;
      line([[i * .065, -.23 - h / 2], [i * .065, -.23 + h / 2]], .025);
    }
  } else if (s.expression === 'thinking') {
    for (let i = 0; i < 3; i++) round(-.115 + i * .09, -.255, .045, .045, .018, Math.floor(phase * 3) % 3 === i ? '#ffffff' : color);
  } else line([[-.08, -.24], [.08, -.24]], .025);
  ctx.restore();
  ctx.beginPath(); ctx.moveTo(-.05, .005); ctx.lineTo(.05, .005); ctx.lineTo(0, .045); ctx.closePath(); ctx.fillStyle = '#526957'; ctx.fill();
  ctx.restore(); ctx.restore();
  ctx.fillStyle = color; ctx.font = 'bold 10px monospace'; ctx.textAlign = 'center';
  ctx.fillText(s.label || s.expression.toUpperCase(), width / 2, height - 17);
  ctx.textAlign = 'left';
}
