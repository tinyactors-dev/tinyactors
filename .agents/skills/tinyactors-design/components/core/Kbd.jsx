import React from 'react';
export function Kbd({ children, ...rest }) {
  return <kbd className="ta-kbd" {...rest}>{children}</kbd>;
}
