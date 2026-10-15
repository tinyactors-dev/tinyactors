import React from 'react';
import { IconButton } from '../core/IconButton.jsx';
export function CodeBlock({ code = '', filename, lang, lineNumbers = true, highlight = [], copyable = true, style }) {
  const lines = String(code).replace(/\n$/, '').split('\n');
  const [copied, setCopied] = React.useState(false);
  const copy = () => { navigator.clipboard?.writeText(code); setCopied(true); setTimeout(() => setCopied(false), 1200); };
  return (
    <div className={'ta-code' + (lineNumbers ? '' : ' ta-code__nolns')} style={style}>
      {(filename || copyable) && (
        <div className="ta-code__head">
          <span>{filename}</span>
          {copyable && <IconButton icon={copied ? 'check' : 'copy'} label="Copy" size="sm" style={{ color: 'var(--code-muted)' }} onClick={copy} />}
        </div>
      )}
      <pre>{lines.map((l, i) => (
        <div key={i} className={'ta-code__line' + (highlight.includes(i + 1) ? ' ta-code__line--hl' : '')}>
          {lineNumbers && <span className="ta-code__ln">{i + 1}</span>}
          <span>{l || ' '}</span>
        </div>
      ))}</pre>
    </div>
  );
}
