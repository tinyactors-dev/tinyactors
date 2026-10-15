import * as React from 'react';
export interface TooltipProps {
  content: React.ReactNode;
  side?: 'top' | 'bottom';
  /** Force visible (docs/specimens). */
  open?: boolean;
  children: React.ReactNode;
}
export declare function Tooltip(props: TooltipProps): JSX.Element;
