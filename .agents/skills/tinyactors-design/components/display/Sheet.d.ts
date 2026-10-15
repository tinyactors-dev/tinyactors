import * as React from 'react';
/**
 * Drafting sheet: ruled double frame with corner registration marks, optional construction grid. The brand's signature container for heroes, diagrams and chart canvases.
 */
export interface SheetProps extends React.HTMLAttributes<HTMLDivElement> {
  /** Fill the frame with the 8/40px construction grid. Default false. */
  grid?: boolean;
  /** Grid scrolls at half speed behind the content as the page scrolls. Website heroes only; off under reduced motion. Requires grid. */
  parallax?: boolean;
  /** Show registration marks + outer margin. false = frame only, flush to parent. Default true. */
  marks?: boolean;
  /** Style for the inner body (padding, layout). */
  bodyStyle?: React.CSSProperties;
  children?: React.ReactNode;
}
export declare function Sheet(props: SheetProps): JSX.Element;
