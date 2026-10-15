import React from 'react';
export function Tooltip({ content, side = 'top', open, children }) {
  return (
    <span className={'ta-tip ta-tip--' + side + (open ? ' ta-tip--open' : '')}>
      {children}
      <span role="tooltip" className="ta-tip__bubble">{content}</span>
    </span>
  );
}
