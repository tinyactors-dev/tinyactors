import * as React from 'react';
export interface MailboxMessage {
  /** Stable key; also printed as #id. */
  id: string | number;
  /** Event type, e.g. "PAY". */
  type: string;
  note?: string;
}
/**
 * Mailbox as a stack receding into depth by age. Newest first: new messages slide in at the front, the oldest drops away at the back when consumed.
 */
export interface MailboxStackProps extends React.HTMLAttributes<HTMLDivElement> {
  /** Newest first. Change the array to animate arrivals and consumption. */
  messages: MailboxMessage[];
  /** Max cards drawn. Default 6. */
  depth?: number;
  width?: number;
  height?: number;
}
export declare function MailboxStack(props: MailboxStackProps): JSX.Element;
