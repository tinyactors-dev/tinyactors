import * as React from 'react';
export interface BadgeProps extends React.HTMLAttributes<HTMLSpanElement> {
  /** Actor lifecycle tone. running pulses. */
  tone?: 'running' | 'waiting' | 'done' | 'error' | 'stopped' | 'neutral';
  /** Leading status dot. Default true. */
  dot?: boolean;
  /** Defaults to the tone name. */
  children?: React.ReactNode;
}
export declare function Badge(props: BadgeProps): JSX.Element;
