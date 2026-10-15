import * as React from 'react';
export interface IconButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  /** Lucide name or node. */
  icon: string | React.ReactNode;
  /** Accessible label; also the native title. Required. */
  label: string;
  variant?: 'primary' | 'secondary' | 'ghost' | 'danger';
  size?: 'sm' | 'md' | 'lg';
}
export declare function IconButton(props: IconButtonProps): JSX.Element;
