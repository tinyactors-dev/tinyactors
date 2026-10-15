import * as React from 'react';
export interface SpanEvent {
  /** Event name (shown as tooltip). */
  name: string;
  /** Absolute time, same unit/clock as Span.start (ms). */
  time: number;
}
export interface Span {
  spanId: string;
  /** Parent span id; omit/null for root spans. Unknown parents render as roots. */
  parentId?: string | null;
  /** Operation name, e.g. "ta_send PAYMENT.OK". */
  name: string;
  /** service.name resource attribute. Drives the color swatch. */
  service: string;
  /** Start time in ms (any epoch; the minimum start becomes 0). */
  start: number;
  /** Duration in ms. */
  duration: number;
  status?: 'ok' | 'error' | 'unset';
  kind?: 'internal' | 'server' | 'client' | 'producer' | 'consumer';
  /** Span events drawn as ticks on the bar. */
  events?: SpanEvent[];
}
/**
 * OpenTelemetry trace waterfall: collapsible span tree on the left, duration bars on a shared time ruler on the right.
 */
export interface SpanTreeProps extends Omit<React.HTMLAttributes<HTMLDivElement>, 'onSelect'> {
  /** Flat list of spans; the tree is built from parentId. */
  spans: Span[];
  selectedId?: string;
  onSelect?: (spanId: string, span: Span) => void;
  /** Span ids collapsed on first render. */
  defaultCollapsed?: string[];
  /** Override service → CSS color. Default hashes service name into --data-1…6 (madder red is reserved for errors). */
  serviceColors?: Record<string, string>;
  /** Width of the tree column in px. Default 320. */
  treeWidth?: number;
  /** Row height in px. Default 26. */
  rowHeight?: number;
  /** Ruler divisions. Default 4. */
  ticks?: number;
  /** Show the time ruler header. Default true. */
  showRuler?: boolean;
}
export declare function SpanTree(props: SpanTreeProps): JSX.Element;
export declare function formatDuration(ms: number): string;
