// Exports Tidewater's procedural lobster boat (dgreenheck/tidewater, MIT, (c) DRG Software Solutions LLC)
// to the mesh file generate_tidewater_boat.py turns into the @OP_TidewaterBoat model. The boat is code, not
// an asset: src/world/BoatModel.js builds it from hull lines, lofts and primitives. This runs that code
// once (plain Node, no GPU) and writes what it built, so the engine build does not need Node.
//
//   node scripts/default-content/export_tidewater_boat.mjs <tidewater checkout> <out .bin.gz>
//
// Exported from tidewater 4811ba48d795197de5621985f404e765c0b7c0ef (2026-09-24). Everything is in
// Tidewater's boat frame: +Z forward, +Y up, +X port, y = 0 the design waterline, metres.
//
// File: gzip of [u32 headerBytes][header JSON][float32/uint32 blobs]. The header lists, per mesh bucket,
// the offsets of positions (3 f32), normals (3 f32), colours (3 f32, linear albedo), aux (4 f32: rough,
// metal, pattern, anim), uv (2 f32: the shaders' pattern coordinates) and indices (u32), plus the anchor points, colliders and hydrostatics.
import { writeFileSync } from 'node:fs';
import { gzipSync } from 'node:zlib';
import { pathToFileURL } from 'node:url';
import { join } from 'node:path';

const [ root, out ] = process.argv.slice( 2 );
if ( ! root || ! out ) {

	console.error( 'usage: node export_tidewater_boat.mjs <tidewater checkout> <out .bin.gz>' );
	process.exit( 2 );

}

const { BoatModel } = await import( pathToFileURL( join( root, 'src/world/BoatModel.js' ) ).href );
const boat = new BoatModel();
boat.setSteering( 0 );
boat.setThrottle( 0 );
boat.group.updateMatrixWorld( true );

const blobs = [];
let offset = 0;
const push = ( typed ) => {

	const bytes = Buffer.from( typed.buffer, typed.byteOffset, typed.byteLength );
	blobs.push( bytes );
	const at = offset;
	offset += bytes.length;
	return at;

};

const v3 = ( v ) => [ v.x, v.y, v.z ];
const meshes = {};
for ( const [ name, mesh ] of Object.entries( boat.meshes ) ) {

	mesh.updateWorldMatrix( true, false );
	const g = mesh.geometry;
	const n = g.attributes.position.count;
	const pos = new Float32Array( n * 3 ), nrm = new Float32Array( n * 3 );
	const m = mesh.matrixWorld.elements; // column-major; the boat group sits at the origin
	const p = g.attributes.position.array, q = g.attributes.normal.array;
	for ( let i = 0; i < n; i ++ ) {

		const x = p[ i * 3 ], y = p[ i * 3 + 1 ], z = p[ i * 3 + 2 ];
		pos[ i * 3 ] = m[ 0 ] * x + m[ 4 ] * y + m[ 8 ] * z + m[ 12 ];
		pos[ i * 3 + 1 ] = m[ 1 ] * x + m[ 5 ] * y + m[ 9 ] * z + m[ 13 ];
		pos[ i * 3 + 2 ] = m[ 2 ] * x + m[ 6 ] * y + m[ 10 ] * z + m[ 14 ];
		const a = q[ i * 3 ], b = q[ i * 3 + 1 ], c = q[ i * 3 + 2 ];
		let nx = m[ 0 ] * a + m[ 4 ] * b + m[ 8 ] * c, ny = m[ 1 ] * a + m[ 5 ] * b + m[ 9 ] * c, nz = m[ 2 ] * a + m[ 6 ] * b + m[ 10 ] * c;
		const l = Math.hypot( nx, ny, nz ) || 1;
		nrm[ i * 3 ] = nx / l; nrm[ i * 3 + 1 ] = ny / l; nrm[ i * 3 + 2 ] = nz / l;

	}

	const col = g.attributes.color ? Float32Array.from( g.attributes.color.array ) : new Float32Array( n * 3 ).fill( 1 );
	const aux = g.attributes.aux ? Float32Array.from( g.attributes.aux.array ) : new Float32Array( n * 4 );
	const uv = g.attributes.uv ? Float32Array.from( g.attributes.uv.array ) : new Float32Array( n * 2 );
	let idx;
	if ( g.index ) idx = Uint32Array.from( g.index.array );
	else idx = Uint32Array.from( { length: n }, ( _, i ) => i );
	meshes[ name ] = { vertices: n, triangles: idx.length / 3, position: push( pos ), normal: push( nrm ), color: push( col ),
		aux: push( aux ), uv: push( uv ), index: push( idx ) };

}

const wheelAxis = [ 0, 0, 1 ];
{

	const e = boat.wheelPivot.matrixWorld.elements;
	wheelAxis[ 0 ] = e[ 8 ]; wheelAxis[ 1 ] = e[ 9 ]; wheelAxis[ 2 ] = e[ 10 ];

}

const header = {
	source: 'dgreenheck/tidewater src/world/BoatModel.js (MIT, (c) DRG Software Solutions LLC)',
	commit: '4811ba48d795197de5621985f404e765c0b7c0ef',
	frame: '+Z forward, +Y up, +X port, y=0 design waterline',
	meshes,
	dimensions: boat.dimensions,
	hydro: { ...boat.hydro, centerOfBuoyancy: v3( boat.hydro.centerOfBuoyancy ), centerOfMass: v3( boat.hydro.centerOfMass ), inertia: v3( boat.hydro.inertia ) },
	anchors: {
		helmEye: v3( boat.helmEye ), boardPoint: v3( boat.boardPoint ), exitPoints: boat.exitPoints.map( v3 ),
		propeller: v3( boat.propeller ), rudder: v3( boat.rudder ),
		wheelCenter: v3( boat.wheelPivot.getWorldPosition( boat.wheelPivot.position.clone() ) ), wheelAxis,
		radarPivot: v3( boat.radarMesh.position ), throttlePivot: v3( boat.throttleMesh.position ),
	},
	colliders: boat.colliders.map( ( c ) => ( { tag: c.tag, center: v3( c.center ), half: v3( c.half ), walkable: c.walkable, solid: c.solid } ) ),
	hullSamples: boat.hullSamples.map( ( s ) => ( { position: v3( s.position ), area: s.area, bottomY: s.bottomY } ) ),
};

const json = Buffer.from( JSON.stringify( header ) );
const len = Buffer.alloc( 4 );
len.writeUInt32LE( json.length );
writeFileSync( out, gzipSync( Buffer.concat( [ len, json, ...blobs ] ), { level: 9 } ) );
let tris = 0;
for ( const m of Object.values( meshes ) ) tris += m.triangles;
console.log( `wrote ${ out }: ${ Object.keys( meshes ).length } meshes, ${ tris } triangles` );
