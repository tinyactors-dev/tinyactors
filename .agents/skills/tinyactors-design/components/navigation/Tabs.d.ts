import * as React from 'react';
export interface TabItem { value: string; label: React.ReactNode; count?: number; }
export interface TabsProps {
  items: Array<string | TabItem>;
  value?: string;
  defaultValue?: string;
  onChange?: (value: string) => void;
  /** underline for page sections; segmented for compact view switches. */
  variant?: 'underline' | 'segmented';
  style?: React.CSSProperties;
}
export declare function Tabs(props: TabsProps): JSX.Element;
