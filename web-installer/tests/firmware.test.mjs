import test from 'node:test';
import assert from 'node:assert/strict';
import {validateBoard,verifyPart} from '../scripts/firmware-guards.mjs';
test('C5 model capacities reject cross-model and unknown hardware',()=>{
 const models=[{name:'headless',flash_sizes:[8388608]},{name:'touch 1.69',flash_sizes:[16777216]},{name:'touch 2.8',flash_sizes:[33554432]}];
 for (let n=0;n<3;n++) for(let m=0;m<3;m++) {
  const id=0x20 | 0x4000 | ((23+n)<<16);
  if(n===m) assert.equal(validateBoard(models[m],'ESP32-C5',id),2**(23+n));else assert.throws(()=>validateBoard(models[m],'ESP32-C5',id));
 }
 assert.throws(()=>validateBoard(models[0],'ESP32-S3',0x174020));
 assert.throws(()=>validateBoard(models[0],'ESP32-C5',0));
 assert.throws(()=>validateBoard(models[0],'ESP32-C5',0xffffff));
});
test('firmware bytes, size and SHA-256 must match before writing',async()=>{
 const data=new TextEncoder().encode('verified firmware');
 const hash=Buffer.from(await crypto.subtle.digest('SHA-256',data)).toString('hex');
 const part={offset:0x10000,bytes:data.length,sha256:hash};
 await verifyPart(part,data);
 await assert.rejects(()=>verifyPart({...part,bytes:data.length+1},data));
 await assert.rejects(()=>verifyPart({...part,sha256:'0'.repeat(64)},data));
 await assert.rejects(()=>verifyPart({...part,offset:-1},data));
});
