import * as React from 'react';
export interface StructField {
  name: string;
  /** C type, shown in tooltip and table. */
  type?: string;
  /** Size in bytes. */
  size: number;
  /** Alignment in bytes. Default: natural (size for 1/2/4/8, 8 for multiples of 8, else 1). */
  align?: number;
  /** Explicit offset; otherwise computed with C layout rules (padding inserted). */
  offset?: number;
  /** Access temperature / role. hot = read on every event (pine tint), atomic = written across cores. Default data. */
  kind?: 'data' | 'hot' | 'warm' | 'cold' | 'ptr' | 'atomic';
  /** Force a madder warning outline (straddling a cache line is flagged automatically). */
  warn?: boolean;
  note?: string;
}
/**
 * Byte-accurate struct layout on cache-line rows: fields sized by bytes, padding hatched, line-straddling fields flagged, sizeof/padding summary. pahole, drawn.
 */
export interface MemoryLayoutProps extends React.HTMLAttributes<HTMLDivElement> {
  /** Struct name, rendered "struct {name}". */
  name?: string;
  fields: StructField[];
  /** No alignment padding (__attribute__((packed))). */
  packed?: boolean;
  /** Cache line size in bytes. Default 64. */
  lineSize?: number;
  /** Bytes per drawn row. Default = lineSize (one row per cache line, max 64). */
  bytesPerRow?: number;
  rowHeight?: number;
  /** pahole-style offset/size/type/name table below. Default false. */
  table?: boolean;
  summary?: boolean;
  legend?: boolean;
}
export declare function MemoryLayout(props: MemoryLayoutProps): JSX.Element;
/** Computes C layout: returns fields with offsets and inserted padding, plus sizeof. */
export declare function layoutStruct(fields: StructField[], opts?: { packed?: boolean }): { fields: (StructField & { offset: number; kind: string; tail?: boolean })[]; size: number };
