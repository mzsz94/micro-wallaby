#!/usr/bin/env node
// Reproducible GIF recorder. The server has no gateway, STT model, or local LLM.
import { spawn } from 'node:child_process';
import { mkdir, readFile, writeFile, rm, stat } from 'node:fs/promises';
import { dirname, resolve, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { setTimeout as delay } from 'node:timers/promises';
import assert from 'node:assert/strict';
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
  return new Promise((accept,reject)=>{const child=spawn(command,args,{stdio:['ignore','ignore','pipe']});let errors='';child.stderr.on('data',data=>errors+=data);child.on('error',reject);child.on('exit',code=>code===0?accept():reject(new Error(`${command}: ${errors}`)));});
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
    const assets={'/demo/':'stage.html','/demo/stage.css':'stage.css','/demo/stage.mjs':'stage.mjs'};
    if(assets[url.pathname]){
      await route.fulfill({body:await readFile(join(source,assets[url.pathname])),contentType:url.pathname.endsWith('.css')?'text/css':url.pathname.endsWith('.mjs')?'text/javascript':'text/html'});
    }else await route.continue();
  });
  // Drive timers, heartbeats and Motion Lab at the exact GIF sample interval.
  await page.clock.install({time:new Date('2026-10-11T00:00:00Z')});
  await page.clock.pauseAt(new Date('2026-10-11T00:00:01Z'));
  await page.goto(origin+'/demo/',{waitUntil:'networkidle'});
  await page.clock.runFor(250);
  const scenes=await page.evaluate(()=>window.demo.scenes);
  const proofs=[];
  const allFrames=join(build,'all-frames');await mkdir(allFrames,{recursive:true});
  let total=0;
  for(let index=0;index<scenes.length;index++){
    const scene=scenes[index];console.log(`Recording ${scene.id}`);
    await page.evaluate(index=>window.demo.load(index),index);
    await page.clock.runFor(150);
    const frames=join(build,scene.id);await mkdir(frames,{recursive:true});
    let proof;
    for(let n=0;n<seconds*fps;n++){
      const time=n/fps;
      proof=await page.evaluate(time=>window.demo.tick(time),time);
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
    proofs.push({...proof,inputMode:'captioned-text',responseEngine:'local-extractive',physicalHardware:false});
    if(!preview){
      const palette=join(frames,'palette.png');
      await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(frames,'%04d.png'),'-vf','palettegen=max_colors=128:stats_mode=diff','-frames:v','1',palette]);
      await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(frames,'%04d.png'),'-i',palette,'-lavfi','paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle','-loop','0',join(media,scene.id+'.gif')]);
      console.log(`Saved ${scene.id}.gif (${(await stat(join(media,scene.id+'.gif'))).size} bytes)`);
    }
  }
  assert.deepEqual(errors,[]);assert.deepEqual(external,[]);
  if(!preview){
    const palette=join(build,'overview-palette.png');
    await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(allFrames,'%04d.png'),'-vf','palettegen=max_colors=128:stats_mode=diff','-frames:v','1',palette]);
    await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(allFrames,'%04d.png'),'-i',palette,'-lavfi','paletteuse=dither=bayer:bayer_scale=4:diff_mode=rectangle','-loop','0',join(media,'military-wallaby-overview.gif')]);
    await run('ffmpeg',['-y','-v','error','-framerate',String(fps),'-i',join(allFrames,'%04d.png'),'-c:v','libx264','-pix_fmt','yuv420p','-crf','20','-movflags','+faststart',join(build,'military-wallaby-demo.mp4')]);
  }
  await writeFile(join(build,preview?'preview-proof.json':'recording-proof.json'),JSON.stringify({fps,secondsPerScene:seconds,viewport:[1120,630],externalRequests:external,browserErrors:errors,scenes:proofs},null,2)+'\n');
  console.log(preview?'Preview verified.':'All GIFs and MP4 encoded; real API responses and simulation checks passed.');
}finally{
  await browser?.close();server.kill('SIGTERM');
  await writeFile(join(build,'server.log'),serverLog);
}
