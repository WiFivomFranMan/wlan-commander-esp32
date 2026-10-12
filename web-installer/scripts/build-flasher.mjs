import {build} from 'esbuild';
import {existsSync,mkdirSync} from 'node:fs';
mkdirSync('docs',{recursive:true});
import {resolve} from 'node:path';
import {writeFileSync} from 'node:fs';
const result = await build({entryPoints:['scripts/flasher.js'],bundle:true,format:'esm',splitting:true,outdir:'public/assets/flasher',minify:true,metafile:true,plugins:[{name:'verified-board-flash',setup(b){b.onResolve({filter:/^@material\/web\/.*-styles\.js$/},args=>{const path=resolve('node_modules',args.path.replace(/\.js$/,'.cssresult.js'));return existsSync(path)?{path}:null});b.onResolve({filter:/^\.\/flash$/},args=>args.importer.endsWith('/esp-web-tools/dist/install-dialog.js')?{path:resolve('scripts/guarded-flash.js')}:null)}}]});
writeFileSync('docs/flasher-bundle.json',JSON.stringify(result.metafile,null,2)+'\n');
