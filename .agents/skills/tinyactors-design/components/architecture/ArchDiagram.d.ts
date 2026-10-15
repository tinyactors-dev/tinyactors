import * as React from 'react';
export interface ArchPort {
  id: string;
  side: 'left' | 'right' | 'top' | 'bottom';
  /** 0–1 along the side. Default 0.5. */
  at?: number;
  /** Pin label printed inside the block edge. */
  label?: string;
}
export interface ArchBlock {
  id: string;
  x: number; y: number; w: number; h: number;
  /** C4 level or memory role. system = double frame, external = dashed, memory = hatched region of RAM, store = hatched + double bottom rule (disk). Default container. */
  kind?: 'system' | 'container' | 'component' | 'external' | 'memory' | 'store';
  name: string;
  /** Technology, rendered in grey mono under the name. */
  tech?: string;
  /** One-line description (needs block height ≥ ~96). */
  note?: string;
  /** Optional uppercase role label, e.g. "SPSC ring". Omit by default: the border already encodes kind. */
  label?: string;
  /** hot = pine outline (the thing being explained); warn = madder outline. */
  emphasis?: 'hot' | 'warn';
  ports?: ArchPort[];
  /** Nested diagram. Clicking the block zooms into it; the parent falls back as context and a path appears for going back up. */
  inner?: { blocks: ArchBlock[]; zones?: ArchZone[]; wires?: ArchWire[]; width?: number; height?: number };
}
export interface ArchZone {
  x: number; y: number; w: number; h: number;
  /** Where things live, e.g. "process · RAM", "core 0 · L1/L2", "disk". */
  label: string;
  /** Right-aligned note, e.g. latency "~100 ns". */
  meta?: string;
  hatch?: boolean;
}
export interface ArchWire {
  /** "blockId.portId" */
  from: string;
  to: string;
  /** call = sync call · stream = queued/async, animated · shared = zero-copy shared memory · copy = memcpy/serialisation (ochre) · dont = anti-pattern (madder, ✕). Default call. */
  kind?: 'call' | 'stream' | 'shared' | 'copy' | 'dont';
  label?: string;
  /** Pine — the path being explained. */
  hot?: boolean;
  /** Arrowheads on both ends. */
  both?: boolean;
  /** Set false to stop the stream animation. */
  animate?: boolean;
  /** X (for horizontal exits) or Y (vertical) of the middle leg. */
  mid?: number;
  /** Manual interior waypoints (orthogonal); overrides auto-routing. */
  via?: [number, number][];
  labelOffset?: [number, number];
}
/**
 * PCB-style architecture drawing: C4 blocks with pin pads, memory zones, and chamfered traces typed by how data moves (call, stream, shared, copy, don't).
 */
export interface ArchDiagramProps extends React.HTMLAttributes<HTMLDivElement> {
  blocks: ArchBlock[];
  zones?: ArchZone[];
  wires?: ArchWire[];
  /** Canvas size in px. Positions are absolute — no auto-layout. */
  width?: number;
  height?: number;
  /** Construction grid behind the canvas. Default true; set false inside a <Sheet grid>. */
  grid?: boolean;
  /** Legend of wire kinds used. Default false: turn on the first time a reader meets the wire vocabulary. */
  legend?: boolean;
  /** Board seen at an angle with extruded blocks; flattens to plan view on hover. Website hero moments only. Default false. */
  tilt?: boolean;
  /** First segment of the dive path. Default "system". */
  rootLabel?: string;
}
export declare function ArchDiagram(props: ArchDiagramProps): JSX.Element;
