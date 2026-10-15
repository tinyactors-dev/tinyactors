import * as React from 'react';
export interface TagProps extends React.HTMLAttributes<HTMLSpanElement> {
  /** Leading unicode glyph in accent color, e.g. "→" for events, "#" for ids. */
  glyph?: string;
  onRemove?: () => void;
  children: React.ReactNode;
}
export declare function Tag(props: TagProps): JSX.Element;
