import * as React from 'react';
export interface DialogProps {
  open?: boolean;
  title: React.ReactNode;
  children?: React.ReactNode;
  /** Right-aligned buttons. */
  footer?: React.ReactNode;
  onClose?: () => void;
  /** Render without the fixed overlay (for docs/cards). */
  inline?: boolean;
  width?: number;
}
export declare function Dialog(props: DialogProps): JSX.Element | null;
