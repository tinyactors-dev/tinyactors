import * as React from 'react';
/** Lucide glyph rendered through a CSS mask so it inherits currentColor. */
export interface IconProps extends React.HTMLAttributes<HTMLSpanElement> {
  /** Lucide icon name in kebab-case, e.g. "play", "git-branch", "circle-dot". */
  name: string;
  /** Pixel size. 16 in UI, 14 in dense rows, 20 in marketing. */
  size?: number;
}
export declare function Icon(props: IconProps): JSX.Element;
