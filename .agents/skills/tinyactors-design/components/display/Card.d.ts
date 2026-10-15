import * as React from 'react';
/**
 * Bordered panel. No shadow; 6px radius; hairline border.
 */
export interface CardProps extends Omit<React.HTMLAttributes<HTMLDivElement>, 'title'> {
  title?: React.ReactNode;
  /** Mono uppercase kicker above the title. */
  eyebrow?: string;
  /** Right-aligned header slot, usually IconButtons. */
  actions?: React.ReactNode;
  /** Hover lift (border + surface). */
  interactive?: boolean;
  /** Earlier versions stacked behind with registration offsets and crop marks (e.g. actor restarts, snapshots). Hover fans them out. Default 0. */
  generations?: number;
  /** Body padding 16px. Default true. */
  padded?: boolean;
  children?: React.ReactNode;
}
export declare function Card(props: CardProps): JSX.Element;
