import * as React from 'react';
/**
 * Fixed-capacity ring buffer (mailbox / stream): slots, occupied items, head (read) and tail (write) pointers, next-write slot, wrap-around trace.
 */
export interface RingBufferProps extends React.HTMLAttributes<HTMLDivElement> {
  name?: string;
  /** e.g. "SPSC", "MPSC". */
  mode?: string;
  capacity?: number;
  /** Read index (consumer). */
  head?: number;
  /** Write index (producer) — the next slot written. */
  tail?: number;
  /** Disambiguates head === tail (full vs empty). */
  full?: boolean;
  /** Labels for occupied slots, in order from head. */
  items?: React.ReactNode[];
  /** e.g. "64 B". */
  slotSize?: string;
  /** Optional name under the tail pointer. Omit when obvious from context. */
  producer?: string;
  /** Optional name under the head pointer. */
  consumer?: string;
}
export declare function RingBuffer(props: RingBufferProps): JSX.Element;
