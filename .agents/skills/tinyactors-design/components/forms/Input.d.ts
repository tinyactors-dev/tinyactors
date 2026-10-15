import * as React from 'react';
export interface InputProps extends Omit<React.InputHTMLAttributes<HTMLInputElement>, 'size' | 'prefix'> {
  /** Uppercase mono label above the field. */
  label?: string;
  hint?: string;
  /** Error text; also turns the border red. */
  error?: string;
  /** Mono text inside the field before the value, e.g. "pid:". */
  prefix?: string;
  /** Lucide icon name inside the field. */
  icon?: string;
  /** Use Plex Mono for the value (ids, event names). */
  mono?: boolean;
  size?: 'sm' | 'md' | 'lg';
}
export declare function Input(props: InputProps): JSX.Element;
