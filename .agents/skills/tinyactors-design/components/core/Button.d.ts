import * as React from 'react';
/**
 * Action button. One primary per view.
 */
export interface ButtonProps extends React.ButtonHTMLAttributes<HTMLButtonElement> {
  variant?: 'primary' | 'secondary' | 'ghost' | 'danger';
  size?: 'sm' | 'md' | 'lg';
  /** Lucide name or node, shown before the label. */
  icon?: string | React.ReactNode;
  iconRight?: string | React.ReactNode;
  children?: React.ReactNode;
}
export declare function Button(props: ButtonProps): JSX.Element;
