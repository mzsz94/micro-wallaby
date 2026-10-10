#!/usr/bin/env node
// Reproducible GIF recorder. The server has no gateway, STT model, or local LLM.
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile, rm, stat } from 'node:fs/promises';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import assert from 'node:assert/strict';
import { createHash } from 'node:crypto';
import { WARNING_AT, CLEAR_AT, FOUND_AT } from './choreography.mjs';
const source=dirname(fileURLToPath(import.meta.url));
const root=resolve(source,'../..');
const build=resolve(root,'build/scenario-gifs');
const media=resolve(source,'media');
const python=process.env.WALLABY_PYTHON||'python3';
const modulePath=process.env.WALLABY_PLAYWRIGHT_MODULE;
const {chromium}=await import(modulePath?pathToFileURL(resolve(modulePath)).href:'playwright');
const preview=process.argv.includes('--preview');
const fps=8, seconds=12, port=Number(process.env.WALLABY_DEMO_PORT||8768);
const origin=`http://127.0.0.1:${port}`;
await mkdir(build,{recursive:true});await mkdir(media,{recursive:true});
await rm(join(build,'data'),{recursive:true,force:true});
const server=spawn(python,['-u','-m','voice_tactical.server','--no-browser','--port',String(port),'--data-dir',join(build,'data')],{cwd:root,stdio:['ignore','pipe','pipe']});
let serverLog='';server.stdout.on('data',data=>serverLog+=data);server.stderr.on('data',data=>serverLog+=data);
async function run(command,args){
  return new Promise((accept,reject)=>{const child=spawn(command,args,{cwd:root,stdio:['ignore','pipe','pipe']});let output='',errors='';child.stdout.on('data',data=>output+=data);child.stderr.on('data',data=>errors+=data);child.on('error',reject);child.on('exit',code=>code===0?accept(output):reject(new Error(`${command}: ${errors}`)));});
}
function verifyMotion(scene,samples){
  const first=samples[0].robot;
  const distance=Math.max(...samples.map(({robot})=>Math.hypot(robot.pose.x-first.pose.x,robot.pose.y-first.pose.y)));
  assert.ok(distance>.2,`${scene.id}: visible model movement must exceed 0.2 m`);
  assert.ok(Math.max(...samples.map(({robot})=>robot.trailLength))>5,`${scene.id}: traveled path missing`);
  const expressions=[...new Set(samples.map(({robot})=>robot.expression))];
  assert.ok(expressions.length>=(scene.kind==='search'?2:3),`${scene.id}: distinct scene expressions missing`);
  const headYaw=samples.map(({robot})=>robot.joints.headYaw);
  if(scene.kind!=='alert')assert.ok(Math.max(...headYaw)-Math.min(...headYaw)>15,`${scene.id}: head must visibly scan or react`);
  if(['alert','search'].includes(scene.kind)){
    const stopAt=scene.kind==='alert'?WARNING_AT:FOUND_AT;
    const stopped=samples.filter(({time})=>time>=stopAt+.25);
    const pose=stopped[0].robot.pose;
    for(const {time,robot} of stopped){
      assert.equal(robot.velocity.linear,0,`${scene.id}: linear velocity after warning`);
      assert.equal(robot.velocity.angular,0,`${scene.id}: angular velocity after warning`);
      assert.deepEqual(robot.pose,pose,`${scene.id}: pose must stay fixed after warning, including clear`);
      assert.equal(robot.alertActive,scene.kind==='search'||time<CLEAR_AT,`${scene.id}: alert latch mismatch`);
    }
  }
  return {displacement_m:Number(distance.toFixed(4)),expressions,head_yaw_range_deg:Number((Math.max(...headYaw)-Math.min(...headYaw)).toFixed(2)),samples};
}
let browser;
try{
  let connected=false;
  for(let i=0;i<80;i++){if(server.exitCode!==null)throw new Error(serverLog);try{const response=await fetch(origin+'/api/status');if(response.ok){connected=true;break;}}catch{}await delay(100);}
  assert.ok(connected,'isolated demo server did not start');
  const status=await(await fetch(origin+'/api/status')).json();
  assert.equal(status.gateway.configured,false);assert.equal(status.llm.enabled,false);
  browser=await chromium.launch({headless:true,args:['--no-sandbox']});
  const page=await browser.newPage({viewport:{width:1120,height:630},deviceScaleFactor:1});
  const errors=[],external=[];page.on('pageerror',error=>errors.push(String(error)));
  await page.route('**/*',async route=>{
    const url=new URL(route.request().url());
    if(url.origin!==origin){external.push(url.origin);await route.abort();return;}
    const assets={'/demo/':'stage.html','/demo/stage.css':'stage.css','/demo/stage.mjs':'stage.mjs','/demo/choreography.mjs':'choreography.mjs'};
    if(assets[url.pathname]){
      await route.fulfill({body:await readFile(join(source,assets[url.pathname])),contentType:url.pathname.endsWith('.css')?'text/css':url.pathname.endsWith('.mjs')?'text/javascript':'text/html'});
    }else await route.continue();
  });
  // Drive timers, heartbeats and Motion Lab at the exact GIF sample interval.
  await page.clock.install({time:new Date('2026-10-11T00:00:00Z')});
  await page.clock.pauseAt(new Date('2026-10-11T00:00:01Z'));
  await page.goto(origin+'/demo/',{waitUntil:'networkidle'});
  await page.clock.runFor(250);
  assert.ok(await page.evaluate(()=>Boolean(document.getElementById('robot-frame').contentWindow.wallabyDemo)),'demo-only renderer bridge must be ready');
  const scenes=await page.evaluate(()=>window.demo.scenes);
  const proofs=[];
  const allFrames=join(build,'all-frames');await mkdir(allFrames,{recursive:true});
  let total=0;
  for(let index=0;index<scenes.length;index++){
    const scene=scenes[index];console.log(`Recording ${scene.id}`);
    await page.evaluate(index=>window.demo.load(index),index);
    await page.clock.runFor(150);
    const frames=join(build,scene.id);await mkdir(frames,{recursive:true});
    let proof;const samples=[];
    for(let n=0;n<seconds*fps;n++){
      const time=n/fps;
      proof=await page.evaluate(time=>window.demo.tick(time),time);
      samples.push({time,robot:proof.robot});
      // Capture intermediate expressions and the detection transition for visual QA.
      if(preview&&[0,16,30,40,56,84].includes(n))await page.screenshot({path:join(build,`${scene.id}-${String(n).padStart(2,'0')}.png`),animations:'allow'});
      if(!preview||n===seconds*fps-1){
        const png=await page.screenshot({animations:'allow'});
        if(preview)await writeFile(join(build,scene.id+'.png'),png);
        else{await writeFile(join(frames,`${String(n).padStart(4,'0')}.png`),png);await writeFile(join(allFrames,`${String(total++).padStart(4,'0')}.png`),png);}
      }
      await page.clock.runFor(1000/fps);
    }
    assert.equal(proof.ready,true);assert.ok(proof.results.length>0);
    assert.ok(await page.evaluate(()=>{
      const note=document.getElementById('note').getBoundingClientRect();
      const response=document.getElementById('response').getBoundingClientRect();
      return !document.getElementById('note').textContent||note.bottom<=response.bottom-7;
    }),'response text must fit the recorded frame');
    const motion=verifyMotion(scene,samples);
    if(scene.kind==='search')assert.ok(proof.events.includes('tower-received'),'discovery must reach local control tower records');
    proofs.push({...proof,inputMode:'captioned-text',responseEngine:'local-extractive',physicalHardware:false,motion});
    if(!preview){
      const palette=join(frames,'palette.png');
      await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(frames,'%04d.png'),'-vf','palettegen=max_colors=128:stats_mode=diff','-frames:v','1',palette]);
      await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(frames,'%04d.png'),'-i',palette,'-lavfi','paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle','-loop','0',join(media,scene.id+'.gif')]);
      console.log(`Saved ${scene.id}.gif (${(await stat(join(media,scene.id+'.gif'))).size} bytes)`);
    }
  }
  const standard=await browser.newPage();
  await standard.goto(origin+'/simulation/',{waitUntil:'networkidle'});
  assert.equal(await standard.evaluate(()=>Boolean(window.wallabyDemo)),false,'ordinary Motion Lab must not expose choreography control');
  assert.equal(await standard.evaluate(()=>document.body.classList.contains('wallaby-demo-mode')),false);
  await standard.close();
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  if(!preview){
    const palette=join(build,'overview-palette.png');
    await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(allFrames,'%04d.png'),'-vf','palettegen=max_colors=128:stats_mode=diff','-frames:v','1',palette]);
    await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(allFrames,'%04d.png'),'-i',palette,'-lavfi','paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle','-loop','0',join(media,'military-wallaby-overview.gif')]);
    await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(allFrames,'%04d.png'),'-c:v','libx264','-pix_fmt','yuv420p','-crf','20','-movflags','+faststart',join(media,'military-wallaby-demo.mp4')]);
    const gifs=[];
    for(const file of [...scenes.map(scene=>scene.id+'.gif'),'military-wallaby-overview.gif']){
      const data=await readFile(join(media,file));
      const info=JSON.parse(await run('ffprobe',['-v','error','-count_frames','-select_streams','v:0','-show_entries','stream=width,height,nb_read_frames,duration','-of','json',join(media,file)])).streams[0];
      const expected=file==='military-wallaby-overview.gif'?fps*seconds*scenes.length:fps*seconds;
      assert.equal(Number(info.nb_read_frames),expected,`${file}: encoded frame count`);
      assert.equal(info.width,1120);assert.equal(info.height,630);
      gifs.push({file,bytes:data.length,frames:Number(info.nb_read_frames),duration_ms:Math.round(Number(info.duration)*1000),sha256:createHash('sha256').update(data).digest('hex')});
    }
    await writeFile(join(source,'verification.json'),JSON.stringify({source_base:(await run('git',['rev-parse','HEAD'])).trim(),resolution:[1120,630],fps,seconds_per_scene:seconds,external_browser_requests:external,browser_errors:errors,verification:['real local API responses','report persisted to isolated SQLite','two orders with unknown second deadline','all scenes have visible actual model paths and changing expressions','motion before warning; zero linear/angular velocity and frozen pose after warning','movement rejected while alert latched; clear leaves robot idle','mock search reaches target; discovery report received by local control tower'],boundaries:{speech_input:'captioned text; no STT performance claim',response:'local rules and evidence retrieval; no LLM model used',knowledge:'development training examples, not official military manuals',robot:'Motion Lab choreography, not autonomous language-command execution or physical hardware',detection:'mock training target; no explosive detection sensor',control_tower:'local report API and SQLite receipt; no external transmission',gif_audio:false},scenes:proofs.map(({scene,motion,events})=>({scene,displacement_m:motion.displacement_m,expressions:motion.expressions,head_yaw_range_deg:motion.head_yaw_range_deg,events})),gifs},null,2)+'\n');
  }
  await writeFile(join(build,preview?'preview-proof.json':'recording-proof.json'),JSON.stringify({fps,secondsPerScene:seconds,viewport:[1120,630],externalRequests:external,browserErrors:errors,scenes:proofs},null,2)+'\n');
  console.log(preview?'Preview verified.':'All GIFs and MP4 encoded; real API responses and simulation checks passed.');
}finally{
  await browser?.close();server.kill('SIGTERM');
  await writeFile(join(build,'server.log'),serverLog);
}
