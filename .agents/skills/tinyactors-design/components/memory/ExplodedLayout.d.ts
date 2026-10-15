import * as React from 'react';
import { StructField } from './MemoryLayout';
export interface ExplodedLayer {
  /** Shown on a leader line when exploded, e.g. "header · 16 B". */
  name: string;
  /** Fields in byte order; kind "pad" draws hatched padding. Width is proportional to size. */
  fields: (Pick<StructField, 'name' | 'size'> & { kind?: StructField['kind'] | 'pad' })[];
}
/**
 * Struct, message or actor split into layers stacked at an axonometric angle. Click toggles collapsed/exploded. For docs and teaching, not dense UI.
 */
export interface ExplodedLayoutProps extends React.HTMLAttributes<HTMLDivElement> {
  /** Top layer first. */
  layers: ExplodedLayer[];
  /** Controlled state. Omit to let clicks toggle. */
  exploded?: boolean;
  defaultExploded?: boolean;
  /** Distance between layers when exploded, px. Default 44. */
  gap?: number;
  /** Footprint of each layer, px. */
  width?: number;
  height?: number;
  /** Height of the viewing stage. Default 300. */
  stageHeight?: number;
}
export declare function ExplodedLayout(props: ExplodedLayoutProps): JSX.Element;
