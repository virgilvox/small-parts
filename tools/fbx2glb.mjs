// FBX -> GLB in Node with three.js's own loader and exporter. Used once for the
// army man: Quaternius's CC0 Soldier_Male from the Ultimate Animated Character
// Pack (3d-assets/opengameart/animated-characters-pack). Every mesh is merged
// to one group and one plastic material, since an army man is one colour.
//   (cd <dir with three@0.169 installed> && node tools/fbx2glb.mjs <in.fbx> <out.glb>)
import { readFileSync, writeFileSync } from 'node:fs';
globalThis.self = globalThis;
globalThis.window = globalThis;
globalThis.document = { createElementNS: () => ({ style: {} }), createElement: () => ({ style: {}, getContext: () => null }) };
class FR { readAsArrayBuffer(b) { b.arrayBuffer().then((a) => { this.result = a; this.onloadend && this.onloadend(); }); } readAsDataURL(b) { b.arrayBuffer().then((a) => { this.result = 'data:application/octet-stream;base64,' + Buffer.from(a).toString('base64'); this.onloadend && this.onloadend(); }); } }
globalThis.FileReader = FR;
const { FBXLoader } = await import('three/examples/jsm/loaders/FBXLoader.js');
const { GLTFExporter } = await import('three/examples/jsm/exporters/GLTFExporter.js');
const [src, dst] = process.argv.slice(2);
const buf = readFileSync(src);
const root = new FBXLoader().parse(buf.buffer.slice(buf.byteOffset, buf.byteOffset + buf.length), '');
console.log('anims', root.animations.map((a) => a.name + ':' + a.duration.toFixed(2)).join(' '));
const THREE = await import('three');
root.traverse((o) => {
  if (!o.isMesh) return;
  o.geometry.clearGroups();
  o.material = new THREE.MeshStandardMaterial({ name: 'plastic', color: 0x4c6a2a, roughness: 0.45, metalness: 0 });
  console.log('mesh', o.name, o.geometry.attributes.position.count);
});
root.traverse((o) => { if (o.isBone && o.parent && !o.parent.isBone) console.log('bone root', o.name, 'parent', o.parent.name); });
const box = new THREE.Box3().setFromObject(root); console.log('size', box.min, box.max, 'scale', root.scale);
new GLTFExporter().parse(root, (r) => { writeFileSync(dst, Buffer.from(r)); console.log('wrote', dst, r.byteLength); }, (e) => console.error(e), { binary: true, animations: root.animations });
