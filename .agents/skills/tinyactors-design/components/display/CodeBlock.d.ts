import * as React from 'react';
export interface CodeBlockProps {
  code: string;
  /** Shown in the header, e.g. "checkout.scxml". */
  filename?: string;
  lang?: string;
  lineNumbers?: boolean;
  /** 1-based line numbers to highlight with a pine gutter bar. */
  highlight?: number[];
  copyable?: boolean;
  style?: React.CSSProperties;
}
export declare function CodeBlock(props: CodeBlockProps): JSX.Element;
