import * as React from 'react';
export interface SelectOption { value: string; label: string; }
export interface SelectProps extends Omit<React.SelectHTMLAttributes<HTMLSelectElement>, 'size'> {
  label?: string;
  hint?: string;
  options: Array<string | SelectOption>;
  size?: 'sm' | 'md' | 'lg';
}
export declare function Select(props: SelectProps): JSX.Element;
