import * as React from 'react';
export interface ToastProps {
  tone?: 'info' | 'success' | 'warning' | 'error';
  title?: React.ReactNode;
  children?: React.ReactNode;
  action?: React.ReactNode;
  onClose?: () => void;
  style?: React.CSSProperties;
}
export declare function Toast(props: ToastProps): JSX.Element;
