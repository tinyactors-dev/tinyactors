/* @ds-bundle: {"format":4,"namespace":"TinyactorsDesignSystem_2f489e","components":[{"name":"WIRE_KINDS","sourcePath":"components/architecture/ArchDiagram.jsx"},{"name":"ArchDiagram","sourcePath":"components/architecture/ArchDiagram.jsx"},{"name":"Button","sourcePath":"components/core/Button.jsx"},{"name":"Icon","sourcePath":"components/core/Icon.jsx"},{"name":"IconButton","sourcePath":"components/core/IconButton.jsx"},{"name":"Kbd","sourcePath":"components/core/Kbd.jsx"},{"name":"Badge","sourcePath":"components/display/Badge.jsx"},{"name":"Card","sourcePath":"components/display/Card.jsx"},{"name":"CodeBlock","sourcePath":"components/display/CodeBlock.jsx"},{"name":"Sheet","sourcePath":"components/display/Sheet.jsx"},{"name":"Tag","sourcePath":"components/display/Tag.jsx"},{"name":"Dialog","sourcePath":"components/feedback/Dialog.jsx"},{"name":"Toast","sourcePath":"components/feedback/Toast.jsx"},{"name":"Tooltip","sourcePath":"components/feedback/Tooltip.jsx"},{"name":"Checkbox","sourcePath":"components/forms/Checkbox.jsx"},{"name":"Input","sourcePath":"components/forms/Input.jsx"},{"name":"Radio","sourcePath":"components/forms/Radio.jsx"},{"name":"Select","sourcePath":"components/forms/Select.jsx"},{"name":"Switch","sourcePath":"components/forms/Switch.jsx"},{"name":"ExplodedLayout","sourcePath":"components/memory/ExplodedLayout.jsx"},{"name":"MailboxStack","sourcePath":"components/memory/MailboxStack.jsx"},{"name":"FIELD_KINDS","sourcePath":"components/memory/MemoryLayout.jsx"},{"name":"MemoryLayout","sourcePath":"components/memory/MemoryLayout.jsx"},{"name":"RingBuffer","sourcePath":"components/memory/RingBuffer.jsx"},{"name":"Tabs","sourcePath":"components/navigation/Tabs.jsx"},{"name":"SpanTree","sourcePath":"components/observability/SpanTree.jsx"}],"sourceHashes":{"components/architecture/ArchDiagram.jsx":"f9a9878c7cf1","components/core/Button.jsx":"2f1cb4733739","components/core/Icon.jsx":"509b2f72767e","components/core/IconButton.jsx":"db250ba7c694","components/core/Kbd.jsx":"f3e9b54c4114","components/display/Badge.jsx":"7efae3e87112","components/display/Card.jsx":"58cb393c397b","components/display/CodeBlock.jsx":"ac6b14c14599","components/display/Sheet.jsx":"b17026b74abb","components/display/Tag.jsx":"12478d35c192","components/feedback/Dialog.jsx":"b428209a1557","components/feedback/Toast.jsx":"bf0b936b487e","components/feedback/Tooltip.jsx":"33a4d842e86b","components/forms/Checkbox.jsx":"097b5c2e79b7","components/forms/Input.jsx":"dd3efc166acd","components/forms/Radio.jsx":"e158e2bae70f","components/forms/Select.jsx":"3354a801950e","components/forms/Switch.jsx":"06d8cadf467c","components/memory/ExplodedLayout.jsx":"8c46cd315c0e","components/memory/MailboxStack.jsx":"b78c54e995e7","components/memory/MemoryLayout.jsx":"851fa58dfe4c","components/memory/RingBuffer.jsx":"d269681d7fbc","components/navigation/Tabs.jsx":"0e10b7f98d99","components/observability/SpanTree.jsx":"fb2729ed6b18","explorations/depth-sketches.js":"00de1ff4928c","explorations/memory-sketches.js":"156241e74e42","scripts/build-bundle.cjs":"d59b34b038f0","ui_kits/inspector/ActorTree.jsx":"29ce8cfabd1c","ui_kits/inspector/ChartView.jsx":"990ea0030570","ui_kits/inspector/EventLog.jsx":"6697efc873e4","ui_kits/inspector/Toolbar.jsx":"06bac5a080e3","ui_kits/inspector/data.js":"7cd3afbd7d44","ui_kits/website/DocsPage.jsx":"0c7cb01e209a","ui_kits/website/Features.jsx":"ebe44731a8e6","ui_kits/website/Header.jsx":"49f844db8b7f","ui_kits/website/Hero.jsx":"2e32944fb5be"},"inlinedExternals":[],"unexposedExports":[{"name":"formatDuration","sourcePath":"components/observability/SpanTree.jsx"},{"name":"layoutStruct","sourcePath":"components/memory/MemoryLayout.jsx"}]} */

(() => {

const __ds_ns = (window.TinyactorsDesignSystem_2f489e = window.TinyactorsDesignSystem_2f489e || {});

const __ds_scope = {};

(__ds_ns.__errors = __ds_ns.__errors || []);

// components/architecture/ArchDiagram.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const DIR = {
  left: [-1, 0],
  right: [1, 0],
  top: [0, -1],
  bottom: [0, 1]
};
const WIRE_KINDS = {
  call: {
    c: 'var(--fg-1)',
    w: 1.25,
    arrow: true,
    legend: 'sync call'
  },
  stream: {
    c: 'var(--fg-1)',
    w: 2.5,
    arrow: true,
    flow: true,
    legend: 'stream / queue'
  },
  shared: {
    c: 'var(--fg-2)',
    w: 1.5,
    dash: '1 3.5',
    pads: true,
    legend: 'shared memory · no copy'
  },
  copy: {
    c: 'var(--state-waiting)',
    w: 1.5,
    dash: '6 3',
    arrow: true,
    legend: 'copy'
  },
  dont: {
    c: 'var(--state-error)',
    w: 1.5,
    dash: '4 3',
    arrow: true,
    x: true,
    legend: "don't"
  }
};
function portPoint(b, p) {
  const at = p.at ?? 0.5;
  if (p.side === 'left') return [b.x, b.y + b.h * at];
  if (p.side === 'right') return [b.x + b.w, b.y + b.h * at];
  if (p.side === 'top') return [b.x + b.w * at, b.y];
  return [b.x + b.w * at, b.y + b.h];
}
function route(a, da, b, db, w) {
  const S = 14;
  const A1 = [a[0] + da[0] * S, a[1] + da[1] * S];
  const B1 = [b[0] + db[0] * S, b[1] + db[1] * S];
  const end = [b[0] + db[0] * 4, b[1] + db[1] * 4];
  if (w.via) return [a, A1, ...w.via, B1, end];
  const ha = da[0] !== 0,
    hb = db[0] !== 0;
  let mid;
  if (ha && hb) {
    const mx = w.mid ?? (A1[0] + B1[0]) / 2;
    mid = [[mx, A1[1]], [mx, B1[1]]];
  } else if (ha && !hb) mid = [[B1[0], A1[1]]];else if (!ha && !hb) {
    const my = w.mid ?? (A1[1] + B1[1]) / 2;
    mid = [[A1[0], my], [B1[0], my]];
  } else mid = [[A1[0], B1[1]]];
  return [a, A1, ...mid, B1, end];
}
function chamfer(pts, r = 7) {
  const p = pts.filter((q, i) => i === 0 || q[0] !== pts[i - 1][0] || q[1] !== pts[i - 1][1]);
  const s = [p[0]];
  for (let i = 1; i < p.length - 1; i++) {
    const a = s[s.length - 1],
      b = p[i],
      c = p[i + 1];
    if (Math.abs((b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])) > 1e-6) s.push(b);
  }
  s.push(p[p.length - 1]);
  let d = `M${s[0][0]} ${s[0][1]}`;
  for (let i = 1; i < s.length - 1; i++) {
    const a = s[i - 1],
      b = s[i],
      c = s[i + 1];
    const l1 = Math.hypot(b[0] - a[0], b[1] - a[1]),
      l2 = Math.hypot(c[0] - b[0], c[1] - b[1]);
    const k = Math.min(r, l1 / 2, l2 / 2);
    d += ` L${b[0] - (b[0] - a[0]) / l1 * k} ${b[1] - (b[1] - a[1]) / l1 * k} L${b[0] + (c[0] - b[0]) / l2 * k} ${b[1] + (c[1] - b[1]) / l2 * k}`;
  }
  const e = s[s.length - 1];
  d += ` L${e[0]} ${e[1]}`;
  let best = 0,
    lp = [(s[0][0] + e[0]) / 2, (s[0][1] + e[1]) / 2],
    horiz = true;
  for (let i = 1; i < s.length; i++) {
    const l = Math.hypot(s[i][0] - s[i - 1][0], s[i][1] - s[i - 1][1]);
    if (l > best) {
      best = l;
      lp = [(s[i][0] + s[i - 1][0]) / 2, (s[i][1] + s[i - 1][1]) / 2];
      horiz = s[i][1] === s[i - 1][1];
    }
  }
  return {
    d,
    lp,
    horiz
  };
}
const BLOCK = {
  system: {
    border: '1.5px solid var(--fg-1)',
    background: 'var(--surface-1)',
    boxShadow: 'inset 0 0 0 3px var(--surface-1), inset 0 0 0 3.5px var(--border-2)'
  },
  container: {
    border: '1.25px solid var(--fg-1)',
    background: 'var(--surface-1)'
  },
  component: {
    border: '1px solid var(--border-strong)',
    background: 'var(--surface-1)'
  },
  external: {
    border: '1.25px dashed var(--fg-3)',
    background: 'var(--bg-app)',
    color: 'var(--fg-2)'
  },
  memory: {
    border: '1.25px solid var(--fg-1)',
    background: 'repeating-linear-gradient(135deg, var(--grid-major) 0 1px, transparent 1px 6px), var(--surface-2)'
  },
  store: {
    border: '1.25px solid var(--fg-1)',
    borderBottom: '4px double var(--fg-1)',
    background: 'repeating-linear-gradient(135deg, var(--grid-major) 0 1px, transparent 1px 6px), var(--surface-2)'
  }
};
const label11 = {
  font: 'var(--type-label)',
  letterSpacing: 'var(--tracking-label)',
  textTransform: 'uppercase'
};
function ArchCanvas({
  blocks = [],
  zones = [],
  wires = [],
  width,
  height,
  grid,
  tilt,
  onDive
}) {
  const uid = 'ad' + React.useId().replace(/[^a-zA-Z0-9]/g, '');
  const byId = Object.fromEntries(blocks.map(b => [b.id, b]));
  const end = ref => {
    const [bid, pid] = ref.split('.');
    const b = byId[bid];
    if (!b) return null;
    const p = (b.ports || []).find(q => q.id === pid) || {
      side: 'right'
    };
    return {
      pt: portPoint(b, p),
      dir: DIR[p.side] || DIR.right
    };
  };
  const drawn = wires.map((w, i) => {
    const A = end(w.from),
      B = end(w.to);
    if (!A || !B) return null;
    const k = WIRE_KINDS[w.kind || 'call'] || WIRE_KINDS.call;
    const color = w.hot ? 'var(--accent)' : k.c;
    return {
      i,
      w,
      k,
      color,
      A,
      B,
      ...chamfer(route(A.pt, A.dir, B.pt, B.dir, w)),
      mk: w.hot ? 'hot' : w.kind || 'call'
    };
  }).filter(Boolean);
  const used = [...new Set(wires.map(w => w.kind || 'call'))];
  const pads = [];
  blocks.forEach(b => (b.ports || []).forEach(p => pads.push({
    key: b.id + '.' + p.id,
    pt: portPoint(b, p)
  })));
  const shared = new Set(drawn.filter(d => d.k.pads).flatMap(d => [d.w.from, d.w.to]));
  return /*#__PURE__*/React.createElement("div", {
    className: "ta-arch__canvas",
    style: {
      position: 'relative',
      width,
      height,
      background: grid ? 'var(--bg-grid), var(--surface-1)' : undefined
    }
  }, zones.map((z, i) => /*#__PURE__*/React.createElement("div", {
    key: i,
    style: {
      position: 'absolute',
      left: z.x,
      top: z.y,
      width: z.w,
      height: z.h,
      boxSizing: 'border-box',
      border: '1px dashed var(--border-strong)',
      background: z.hatch ? 'repeating-linear-gradient(135deg, var(--grid-minor) 0 1px, transparent 1px 7px)' : undefined
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      position: 'absolute',
      left: 8,
      top: 7,
      display: 'flex',
      alignItems: 'baseline',
      gap: 8,
      whiteSpace: 'nowrap'
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      ...label11,
      color: 'var(--fg-3)'
    }
  }, z.label), z.meta && /*#__PURE__*/React.createElement("span", {
    style: {
      font: '400 10px/1 var(--font-mono)',
      color: 'var(--fg-4)'
    }
  }, z.meta)))), /*#__PURE__*/React.createElement("svg", {
    width: width,
    height: height,
    style: {
      position: 'absolute',
      inset: 0,
      overflow: 'visible',
      pointerEvents: 'none'
    }
  }, /*#__PURE__*/React.createElement("defs", null, [...Object.keys(WIRE_KINDS), 'hot'].map(kk => /*#__PURE__*/React.createElement("marker", {
    key: kk,
    id: `${uid}-${kk}`,
    viewBox: "0 0 10 10",
    refX: "8.5",
    refY: "5",
    markerWidth: "10",
    markerHeight: "10",
    markerUnits: "userSpaceOnUse",
    orient: "auto-start-reverse"
  }, /*#__PURE__*/React.createElement("path", {
    d: "M1 1.5 L8.5 5 L1 8.5",
    fill: "none",
    style: {
      stroke: kk === 'hot' ? 'var(--accent)' : WIRE_KINDS[kk].c,
      strokeWidth: 1.4
    }
  })))), drawn.map(({
    i,
    w,
    k,
    color,
    d,
    mk
  }) => /*#__PURE__*/React.createElement("g", {
    key: i
  }, /*#__PURE__*/React.createElement("path", {
    d: d,
    fill: "none",
    strokeLinecap: k.dash === '1 3.5' ? 'round' : 'butt',
    strokeLinejoin: "round",
    markerEnd: k.arrow ? `url(#${uid}-${mk})` : undefined,
    markerStart: k.arrow && w.both ? `url(#${uid}-${mk})` : undefined,
    style: {
      stroke: color,
      strokeWidth: w.hot && k.w < 2 ? 1.75 : k.w,
      strokeDasharray: k.dash
    }
  }), k.flow && w.animate !== false && /*#__PURE__*/React.createElement("path", {
    className: "ta-arch__flow",
    d: d,
    fill: "none",
    style: {
      stroke: 'var(--surface-1)',
      strokeWidth: 1,
      strokeDasharray: '2 8'
    }
  })))), tilt && blocks.map(b => /*#__PURE__*/React.createElement(React.Fragment, {
    key: 'side' + b.id
  }, /*#__PURE__*/React.createElement("span", {
    className: "ta-arch__side ta-arch__side--s",
    style: {
      left: b.x,
      top: b.y + b.h,
      width: b.w,
      height: 14
    }
  }), /*#__PURE__*/React.createElement("span", {
    className: "ta-arch__side ta-arch__side--e",
    style: {
      left: b.x + b.w,
      top: b.y,
      width: 14,
      height: b.h
    }
  }), /*#__PURE__*/React.createElement("span", {
    className: "ta-arch__side ta-arch__side--w",
    style: {
      left: b.x - 14,
      top: b.y,
      width: 14,
      height: b.h
    }
  }))), blocks.map(b => {
    const kind = BLOCK[b.kind] ? b.kind : 'container';
    const emph = b.emphasis === 'hot' ? 'var(--accent)' : b.emphasis === 'warn' ? 'var(--state-error)' : null;
    return /*#__PURE__*/React.createElement("div", {
      key: b.id,
      title: b.note,
      className: 'ta-arch__block' + (b.inner && onDive ? ' ta-arch__block--dive' : ''),
      onClick: b.inner && onDive ? () => onDive(b) : undefined,
      style: {
        position: 'absolute',
        left: b.x,
        top: b.y,
        width: b.w,
        height: b.h,
        boxSizing: 'border-box',
        padding: '9px 12px',
        display: 'flex',
        flexDirection: 'column',
        gap: 3,
        overflow: 'hidden',
        color: 'var(--fg-1)',
        ...BLOCK[kind],
        ...(emph ? {
          borderColor: emph
        } : null)
      }
    }, b.label && /*#__PURE__*/React.createElement("span", {
      style: {
        ...label11,
        fontSize: 10,
        color: emph || 'var(--fg-4)',
        whiteSpace: 'nowrap'
      }
    }, b.label), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 13px/1.15 var(--font-mono)',
        whiteSpace: 'nowrap',
        overflow: 'hidden',
        textOverflow: 'ellipsis'
      }
    }, b.name), b.tech && b.h >= 64 && /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 11px/1.2 var(--font-mono)',
        color: 'var(--fg-3)',
        whiteSpace: 'nowrap',
        overflow: 'hidden',
        textOverflow: 'ellipsis'
      }
    }, b.tech), b.note && b.h >= 96 && /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 12px/1.35 var(--font-sans)',
        color: 'var(--fg-2)',
        marginTop: 2,
        textWrap: 'pretty'
      }
    }, b.note), (b.ports || []).filter(p => p.label).map(p => {
      const at = (p.at ?? 0.5) * 100 + '%';
      const pos = p.side === 'left' ? {
        left: 7,
        top: at,
        transform: 'translateY(-50%)'
      } : p.side === 'right' ? {
        right: 7,
        top: at,
        transform: 'translateY(-50%)'
      } : p.side === 'top' ? {
        top: 3,
        left: at,
        transform: 'translateX(-50%)'
      } : {
        bottom: 3,
        left: at,
        transform: 'translateX(-50%)'
      };
      return /*#__PURE__*/React.createElement("span", {
        key: p.id,
        style: {
          position: 'absolute',
          ...pos,
          font: '400 10px/1 var(--font-mono)',
          color: 'var(--fg-3)',
          whiteSpace: 'nowrap',
          background: 'inherit'
        }
      }, p.label);
    }));
  }), /*#__PURE__*/React.createElement("svg", {
    width: width,
    height: height,
    style: {
      position: 'absolute',
      inset: 0,
      overflow: 'visible',
      pointerEvents: 'none'
    }
  }, pads.map(({
    key,
    pt
  }) => /*#__PURE__*/React.createElement("circle", {
    key: key,
    cx: pt[0],
    cy: pt[1],
    r: "3.5",
    style: {
      fill: shared.has(key) ? 'var(--fg-1)' : 'var(--surface-1)',
      stroke: 'var(--fg-1)',
      strokeWidth: 1.25
    }
  })), drawn.map(({
    i,
    w,
    k,
    color,
    lp,
    horiz
  }) => {
    const [dx, dy] = w.labelOffset || [0, 0];
    const tx = lp[0] + (horiz ? 0 : 8) + dx,
      ty = lp[1] + (horiz ? -8 : 4) + dy;
    return /*#__PURE__*/React.createElement("g", {
      key: i
    }, k.x && /*#__PURE__*/React.createElement("g", {
      transform: `translate(${lp[0]} ${lp[1]})`
    }, /*#__PURE__*/React.createElement("circle", {
      r: "7",
      style: {
        fill: 'var(--surface-1)',
        stroke: color,
        strokeWidth: 1.25
      }
    }), /*#__PURE__*/React.createElement("path", {
      d: "M-3 -3 L3 3 M3 -3 L-3 3",
      style: {
        stroke: color,
        strokeWidth: 1.4
      }
    })), w.label && /*#__PURE__*/React.createElement("text", {
      x: k.x && horiz ? tx : k.x ? tx + 4 : tx,
      y: k.x && horiz ? ty - 4 : ty,
      textAnchor: horiz ? 'middle' : 'start',
      style: {
        font: '400 11px var(--font-mono)',
        fill: w.hot ? 'var(--accent-fg)' : k.x ? 'var(--state-error)' : 'var(--fg-2)',
        paintOrder: 'stroke',
        stroke: 'var(--surface-1)',
        strokeWidth: 4,
        strokeLinejoin: 'round'
      }
    }, w.label));
  })));
}
function ArchDiagram({
  blocks = [],
  zones = [],
  wires = [],
  width = 720,
  height = 420,
  grid = true,
  legend = false,
  tilt = false,
  rootLabel = 'system',
  style,
  ...rest
}) {
  const [path, setPath] = React.useState([]);
  const [depth, setDepth] = React.useState(0);
  const timer = React.useRef(0);
  React.useEffect(() => () => clearTimeout(timer.current), []);
  const dive = b => {
    clearTimeout(timer.current);
    setPath(p => [...p.slice(0, depth), b]);
    requestAnimationFrame(() => requestAnimationFrame(() => setDepth(d => d + 1)));
  };
  const up = to => {
    setDepth(to);
    clearTimeout(timer.current);
    timer.current = setTimeout(() => setPath(p => p.slice(0, to)), 900);
  };
  const levels = [{
    blocks,
    zones,
    wires
  }, ...path.map(b => b.inner)];
  const cur = levels[Math.min(depth, levels.length - 1)] || levels[0];
  const used = [...new Set((cur.wires || []).map(w => w.kind || 'call'))];
  const origin = b => b ? `${b.x + b.w / 2}px ${b.y + b.h / 2}px` : '50% 50%';
  return /*#__PURE__*/React.createElement("div", _extends({
    className: 'ta-arch' + (tilt ? ' ta-arch--tilt' : ''),
    style: {
      minWidth: 0,
      overflowX: tilt ? 'visible' : 'auto',
      overflowY: tilt ? 'visible' : 'hidden',
      ...style
    }
  }, rest), /*#__PURE__*/React.createElement("div", {
    style: {
      position: 'relative',
      width,
      height,
      overflow: path.length ? 'hidden' : 'visible'
    }
  }, path.length > 0 && /*#__PURE__*/React.createElement("nav", {
    className: "ta-arch__crumb",
    "aria-label": "Diagram level"
  }, [rootLabel, ...path.map(b => b.name)].slice(0, depth + 1).map((n, i, a) => i < a.length - 1 ? /*#__PURE__*/React.createElement(React.Fragment, {
    key: i
  }, /*#__PURE__*/React.createElement("button", {
    type: "button",
    onClick: () => up(i)
  }, n), /*#__PURE__*/React.createElement("span", null, "/")) : /*#__PURE__*/React.createElement("b", {
    key: i
  }, n))), levels.map((l, i) => /*#__PURE__*/React.createElement("div", {
    key: i,
    className: 'ta-arch__layer' + (i < depth ? ' is-away' : i > depth ? ' is-gone' : ''),
    "aria-hidden": i !== depth,
    style: {
      transformOrigin: origin(i < depth ? path[i] : path[i - 1])
    }
  }, /*#__PURE__*/React.createElement(ArchCanvas, {
    blocks: l.blocks || [],
    zones: l.zones || [],
    wires: l.wires || [],
    width: l.width || width,
    height: l.height || height,
    grid: grid,
    tilt: tilt,
    onDive: i === depth ? dive : undefined
  })))), legend && used.length > 0 && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      flexWrap: 'wrap',
      gap: '6px 18px',
      padding: '10px 2px 0',
      font: '400 11px/1 var(--font-mono)',
      color: 'var(--fg-2)'
    }
  }, used.map(kk => {
    const k = WIRE_KINDS[kk] || WIRE_KINDS.call;
    return /*#__PURE__*/React.createElement("span", {
      key: kk,
      style: {
        display: 'inline-flex',
        alignItems: 'center',
        gap: 7,
        whiteSpace: 'nowrap'
      }
    }, /*#__PURE__*/React.createElement("svg", {
      width: "26",
      height: "10"
    }, /*#__PURE__*/React.createElement("line", {
      x1: "1",
      y1: "5",
      x2: "25",
      y2: "5",
      style: {
        stroke: k.c,
        strokeWidth: k.w,
        strokeDasharray: k.dash,
        strokeLinecap: k.pads ? 'round' : 'butt'
      }
    })), k.legend);
  })));
}
Object.assign(__ds_scope, { WIRE_KINDS, ArchDiagram });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/architecture/ArchDiagram.jsx", error: String((e && e.message) || e) }); }

// components/core/Icon.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const CDN = 'https://unpkg.com/lucide-static@0.460.0/icons/';
const cache = new Map();
function load(name) {
  if (!cache.has(name)) cache.set(name, fetch(CDN + name + '.svg').then(r => r.ok ? r.text() : '').then(t => t.replace(/\s(width|height)="[^"]*"/g, ' width="100%" height="100%"').replace(/<!--[\s\S]*?-->/g, '')).catch(() => ''));
  return cache.get(name);
}
function Icon({
  name,
  size = 16,
  strokeWidth,
  style,
  className,
  ...rest
}) {
  const [svg, setSvg] = React.useState('');
  React.useEffect(() => {
    let on = true;
    load(name).then(t => on && setSvg(strokeWidth ? t.replace(/stroke-width="[^"]*"/, 'stroke-width="' + strokeWidth + '"') : t));
    return () => {
      on = false;
    };
  }, [name, strokeWidth]);
  return /*#__PURE__*/React.createElement("span", _extends({
    "aria-hidden": "true",
    className: 'ta-icon' + (className ? ' ' + className : ''),
    style: {
      width: size,
      height: size,
      ...style
    },
    dangerouslySetInnerHTML: {
      __html: svg
    }
  }, rest));
}
Object.assign(__ds_scope, { Icon });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Icon.jsx", error: String((e && e.message) || e) }); }

// components/core/Button.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Button({
  variant = 'secondary',
  size = 'md',
  icon,
  iconRight,
  children,
  className,
  type = 'button',
  ...rest
}) {
  const is = size === 'sm' ? 14 : 16;
  return /*#__PURE__*/React.createElement("button", _extends({
    type: type,
    className: cx('ta-btn', 'ta-btn--' + variant, size !== 'md' && 'ta-btn--' + size, className)
  }, rest), icon && (typeof icon === 'string' ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: is
  }) : icon), children, iconRight && (typeof iconRight === 'string' ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: iconRight,
    size: is
  }) : iconRight));
}
Object.assign(__ds_scope, { Button });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Button.jsx", error: String((e && e.message) || e) }); }

// components/core/IconButton.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function IconButton({
  icon,
  label,
  variant = 'ghost',
  size = 'md',
  className,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("button", _extends({
    type: "button",
    "aria-label": label,
    title: label,
    className: cx('ta-btn', 'ta-btn--icon', 'ta-btn--' + variant, size !== 'md' && 'ta-btn--' + size, className)
  }, rest), typeof icon === 'string' ? /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: size === 'sm' ? 14 : 16
  }) : icon);
}
Object.assign(__ds_scope, { IconButton });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/IconButton.jsx", error: String((e && e.message) || e) }); }

// components/core/Kbd.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
function Kbd({
  children,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("kbd", _extends({
    className: "ta-kbd"
  }, rest), children);
}
Object.assign(__ds_scope, { Kbd });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/core/Kbd.jsx", error: String((e && e.message) || e) }); }

// components/display/Badge.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const MAP = {
  running: ['var(--state-running)', 'var(--state-running-bg)'],
  waiting: ['var(--state-waiting)', 'var(--state-waiting-bg)'],
  done: ['var(--state-done)', 'var(--state-done-bg)'],
  error: ['var(--state-error)', 'var(--state-error-bg)'],
  stopped: ['var(--state-stopped)', 'var(--state-stopped-bg)'],
  neutral: ['var(--fg-2)', 'var(--surface-3)']
};
function Badge({
  tone = 'neutral',
  dot = true,
  children,
  style,
  ...rest
}) {
  const [c, b] = MAP[tone] || MAP.neutral;
  return /*#__PURE__*/React.createElement("span", _extends({
    className: 'ta-badge ta-badge--' + tone + (children == null ? ' ta-badge--dot' : ''),
    title: children == null ? tone : undefined,
    "aria-label": children == null ? tone : undefined,
    style: {
      '--c': c,
      '--b': b,
      ...style
    }
  }, rest), dot && /*#__PURE__*/React.createElement("span", {
    className: "ta-badge__dot"
  }), children);
}
Object.assign(__ds_scope, { Badge });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/display/Badge.jsx", error: String((e && e.message) || e) }); }

// components/display/Card.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Card({
  title,
  eyebrow,
  actions,
  children,
  interactive,
  padded = true,
  generations = 0,
  className,
  ...rest
}) {
  const card = /*#__PURE__*/React.createElement("div", _extends({
    className: cx('ta-card', interactive && 'ta-card--interactive', className)
  }, rest), (title || eyebrow || actions) && /*#__PURE__*/React.createElement("div", {
    className: "ta-card__head"
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      flexDirection: 'column',
      gap: 6,
      minWidth: 0,
      flex: 1
    }
  }, eyebrow && /*#__PURE__*/React.createElement("span", {
    className: "ta-card__eyebrow"
  }, eyebrow), title && /*#__PURE__*/React.createElement("span", {
    className: "ta-card__title"
  }, title)), actions && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      gap: 4
    }
  }, actions)), /*#__PURE__*/React.createElement("div", {
    className: padded ? 'ta-card__body' : undefined
  }, children));
  if (!generations) return card;
  return /*#__PURE__*/React.createElement("div", {
    className: "ta-gens",
    style: {
      '--n': generations
    }
  }, Array.from({
    length: generations
  }, (_, k) => /*#__PURE__*/React.createElement("span", {
    key: k,
    className: "ta-gens__sheet",
    style: {
      '--i': generations - k
    },
    "aria-hidden": "true"
  })), /*#__PURE__*/React.createElement("div", {
    className: "ta-gens__front"
  }, card, /*#__PURE__*/React.createElement("span", {
    className: "ta-gens__mark ta-gens__mark--tl",
    "aria-hidden": "true"
  }), /*#__PURE__*/React.createElement("span", {
    className: "ta-gens__mark ta-gens__mark--br",
    "aria-hidden": "true"
  })));
}
Object.assign(__ds_scope, { Card });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/display/Card.jsx", error: String((e && e.message) || e) }); }

// components/display/CodeBlock.jsx
try { (() => {
function CodeBlock({
  code = '',
  filename,
  lang,
  lineNumbers = true,
  highlight = [],
  copyable = true,
  style
}) {
  const lines = String(code).replace(/\n$/, '').split('\n');
  const [copied, setCopied] = React.useState(false);
  const copy = () => {
    navigator.clipboard?.writeText(code);
    setCopied(true);
    setTimeout(() => setCopied(false), 1200);
  };
  return /*#__PURE__*/React.createElement("div", {
    className: 'ta-code' + (lineNumbers ? '' : ' ta-code__nolns'),
    style: style
  }, (filename || copyable) && /*#__PURE__*/React.createElement("div", {
    className: "ta-code__head"
  }, /*#__PURE__*/React.createElement("span", null, filename), copyable && /*#__PURE__*/React.createElement(__ds_scope.IconButton, {
    icon: copied ? 'check' : 'copy',
    label: "Copy",
    size: "sm",
    style: {
      color: 'var(--code-muted)'
    },
    onClick: copy
  })), /*#__PURE__*/React.createElement("pre", null, lines.map((l, i) => /*#__PURE__*/React.createElement("div", {
    key: i,
    className: 'ta-code__line' + (highlight.includes(i + 1) ? ' ta-code__line--hl' : '')
  }, lineNumbers && /*#__PURE__*/React.createElement("span", {
    className: "ta-code__ln"
  }, i + 1), /*#__PURE__*/React.createElement("span", null, l || ' ')))));
}
Object.assign(__ds_scope, { CodeBlock });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/display/CodeBlock.jsx", error: String((e && e.message) || e) }); }

// components/display/Sheet.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Sheet({
  children,
  grid = false,
  marks = true,
  parallax = false,
  className,
  style,
  bodyStyle,
  ...rest
}) {
  const ref = React.useRef(null);
  React.useEffect(() => {
    if (!parallax || !grid || !ref.current) return;
    const el = ref.current,
      mq = matchMedia('(prefers-reduced-motion: reduce)');
    let raf = 0;
    const upd = () => {
      raf = 0;
      if (mq.matches) {
        el.style.removeProperty('--ta-py');
        return;
      }
      el.style.setProperty('--ta-py', (-el.getBoundingClientRect().top * 0.5).toFixed(1) + 'px');
    };
    const on = () => {
      if (!raf) raf = requestAnimationFrame(upd);
    };
    upd();
    window.addEventListener('scroll', on, {
      passive: true
    });
    window.addEventListener('resize', on);
    return () => {
      window.removeEventListener('scroll', on);
      window.removeEventListener('resize', on);
      cancelAnimationFrame(raf);
    };
  }, [parallax, grid]);
  return /*#__PURE__*/React.createElement("div", _extends({
    ref: ref,
    className: cx('ta-sheet', grid && 'ta-sheet--grid', grid && parallax && 'ta-sheet--parallax', !marks && 'ta-sheet--flush', className),
    style: style
  }, rest), marks && ['tl', 'tr', 'bl', 'br'].map(p => /*#__PURE__*/React.createElement("span", {
    key: p,
    className: 'ta-sheet__reg ta-sheet__reg--' + p,
    "aria-hidden": "true"
  }, /*#__PURE__*/React.createElement("i", null))), /*#__PURE__*/React.createElement("div", {
    className: "ta-sheet__frame"
  }, /*#__PURE__*/React.createElement("div", {
    className: "ta-sheet__body",
    style: bodyStyle
  }, children)));
}
Object.assign(__ds_scope, { Sheet });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/display/Sheet.jsx", error: String((e && e.message) || e) }); }

// components/display/Tag.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
function Tag({
  children,
  glyph,
  onRemove,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("span", _extends({
    className: 'ta-tag' + (onRemove ? ' ta-tag--removable' : '')
  }, rest), glyph && /*#__PURE__*/React.createElement("span", {
    className: "ta-tag__glyph"
  }, glyph), children, onRemove && /*#__PURE__*/React.createElement("button", {
    "aria-label": "Remove",
    onClick: onRemove
  }, /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: "x",
    size: 12
  })));
}
Object.assign(__ds_scope, { Tag });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/display/Tag.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Dialog.jsx
try { (() => {
function Dialog({
  open = true,
  title,
  children,
  footer,
  onClose,
  inline,
  width
}) {
  if (!open) return null;
  const box = /*#__PURE__*/React.createElement("div", {
    className: "ta-dialog",
    role: "dialog",
    "aria-modal": "true",
    style: width ? {
      maxWidth: width
    } : undefined,
    onClick: e => e.stopPropagation()
  }, /*#__PURE__*/React.createElement("div", {
    className: "ta-dialog__head"
  }, /*#__PURE__*/React.createElement("span", {
    className: "ta-dialog__title"
  }, title), onClose && /*#__PURE__*/React.createElement(__ds_scope.IconButton, {
    icon: "x",
    label: "Close",
    size: "sm",
    onClick: onClose
  })), /*#__PURE__*/React.createElement("div", {
    className: "ta-dialog__body"
  }, children), footer && /*#__PURE__*/React.createElement("div", {
    className: "ta-dialog__foot"
  }, footer));
  if (inline) return box;
  return /*#__PURE__*/React.createElement("div", {
    className: "ta-overlay",
    onClick: onClose
  }, box);
}
Object.assign(__ds_scope, { Dialog });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Dialog.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Toast.jsx
try { (() => {
const C = {
  info: 'var(--state-running)',
  success: 'var(--state-done)',
  warning: 'var(--state-waiting)',
  error: 'var(--state-error)'
};
function Toast({
  tone = 'info',
  title,
  children,
  action,
  onClose,
  style
}) {
  return /*#__PURE__*/React.createElement("div", {
    className: "ta-toast",
    role: "status",
    style: {
      '--c': C[tone],
      ...style
    }
  }, /*#__PURE__*/React.createElement("span", {
    className: "ta-toast__mark"
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      flex: 1,
      minWidth: 0
    }
  }, title && /*#__PURE__*/React.createElement("div", {
    className: "ta-toast__title"
  }, title), children && /*#__PURE__*/React.createElement("div", {
    className: "ta-toast__msg"
  }, children), action && /*#__PURE__*/React.createElement("div", {
    style: {
      marginTop: 10
    }
  }, action)), onClose && /*#__PURE__*/React.createElement(__ds_scope.IconButton, {
    icon: "x",
    label: "Dismiss",
    size: "sm",
    onClick: onClose
  }));
}
Object.assign(__ds_scope, { Toast });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Toast.jsx", error: String((e && e.message) || e) }); }

// components/feedback/Tooltip.jsx
try { (() => {
function Tooltip({
  content,
  side = 'top',
  open,
  children
}) {
  return /*#__PURE__*/React.createElement("span", {
    className: 'ta-tip ta-tip--' + side + (open ? ' ta-tip--open' : '')
  }, children, /*#__PURE__*/React.createElement("span", {
    role: "tooltip",
    className: "ta-tip__bubble"
  }, content));
}
Object.assign(__ds_scope, { Tooltip });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/feedback/Tooltip.jsx", error: String((e && e.message) || e) }); }

// components/forms/Checkbox.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Checkbox({
  label,
  disabled,
  className,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("label", {
    className: cx('ta-check', disabled && 'ta-check--disabled', className)
  }, /*#__PURE__*/React.createElement("input", _extends({
    type: "checkbox",
    disabled: disabled
  }, rest)), /*#__PURE__*/React.createElement("span", {
    className: "ta-check__box"
  }), label && /*#__PURE__*/React.createElement("span", null, label));
}
Object.assign(__ds_scope, { Checkbox });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Checkbox.jsx", error: String((e && e.message) || e) }); }

// components/forms/Input.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Input({
  label,
  hint,
  error,
  prefix,
  icon,
  mono,
  size = 'md',
  disabled,
  id,
  className,
  style,
  ...rest
}) {
  const fid = id || (label ? 'in-' + String(label).toLowerCase().replace(/\W+/g, '-') : undefined);
  return /*#__PURE__*/React.createElement("div", {
    className: cx('ta-field', className),
    style: style
  }, label && /*#__PURE__*/React.createElement("label", {
    className: "ta-label",
    htmlFor: fid
  }, label), /*#__PURE__*/React.createElement("div", {
    className: cx('ta-control', error && 'ta-control--error', mono && 'ta-control--mono', size !== 'md' && 'ta-control--' + size, disabled && 'ta-control--disabled')
  }, icon && /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: icon,
    size: size === 'sm' ? 14 : 16,
    style: {
      color: 'var(--fg-3)'
    }
  }), prefix && /*#__PURE__*/React.createElement("span", {
    className: "ta-prefix"
  }, prefix), /*#__PURE__*/React.createElement("input", _extends({
    id: fid,
    disabled: disabled
  }, rest))), (error || hint) && /*#__PURE__*/React.createElement("span", {
    className: cx('ta-hint', error && 'ta-hint--error')
  }, error || hint));
}
Object.assign(__ds_scope, { Input });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Input.jsx", error: String((e && e.message) || e) }); }

// components/forms/Radio.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Radio({
  label,
  disabled,
  className,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("label", {
    className: cx('ta-check', 'ta-check--radio', disabled && 'ta-check--disabled', className)
  }, /*#__PURE__*/React.createElement("input", _extends({
    type: "radio",
    disabled: disabled
  }, rest)), /*#__PURE__*/React.createElement("span", {
    className: "ta-check__box"
  }), label && /*#__PURE__*/React.createElement("span", null, label));
}
Object.assign(__ds_scope, { Radio });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Radio.jsx", error: String((e && e.message) || e) }); }

// components/forms/Select.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Select({
  label,
  hint,
  options = [],
  size = 'md',
  disabled,
  id,
  className,
  style,
  ...rest
}) {
  const fid = id || (label ? 'sel-' + String(label).toLowerCase().replace(/\W+/g, '-') : undefined);
  return /*#__PURE__*/React.createElement("div", {
    className: cx('ta-field', className),
    style: style
  }, label && /*#__PURE__*/React.createElement("label", {
    className: "ta-label",
    htmlFor: fid
  }, label), /*#__PURE__*/React.createElement("div", {
    className: cx('ta-control', size !== 'md' && 'ta-control--' + size, disabled && 'ta-control--disabled')
  }, /*#__PURE__*/React.createElement("select", _extends({
    id: fid,
    disabled: disabled
  }, rest), options.map(o => typeof o === 'string' ? /*#__PURE__*/React.createElement("option", {
    key: o,
    value: o
  }, o) : /*#__PURE__*/React.createElement("option", {
    key: o.value,
    value: o.value
  }, o.label))), /*#__PURE__*/React.createElement(__ds_scope.Icon, {
    name: "chevrons-up-down",
    size: size === 'sm' ? 14 : 16,
    style: {
      color: 'var(--fg-3)',
      pointerEvents: 'none'
    }
  })), hint && /*#__PURE__*/React.createElement("span", {
    className: "ta-hint"
  }, hint));
}
Object.assign(__ds_scope, { Select });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Select.jsx", error: String((e && e.message) || e) }); }

// components/forms/Switch.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const cx = (...a) => a.filter(Boolean).join(' ');
function Switch({
  label,
  disabled,
  className,
  ...rest
}) {
  return /*#__PURE__*/React.createElement("label", {
    className: cx('ta-switch', disabled && 'ta-switch--disabled', className)
  }, /*#__PURE__*/React.createElement("input", _extends({
    type: "checkbox",
    role: "switch",
    disabled: disabled
  }, rest)), /*#__PURE__*/React.createElement("span", {
    className: "ta-switch__track"
  }), label && /*#__PURE__*/React.createElement("span", null, label));
}
Object.assign(__ds_scope, { Switch });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/forms/Switch.jsx", error: String((e && e.message) || e) }); }

// components/memory/MailboxStack.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
function MailboxStack({
  messages = [],
  depth = 6,
  width = 240,
  height = 260,
  style,
  ...rest
}) {
  const [items, setItems] = React.useState(() => messages.slice(0, depth).map((m, i) => ({
    m,
    st: 'idle',
    i
  })));
  React.useEffect(() => {
    const want = messages.slice(0, depth);
    const ids = new Set(want.map(m => m.id));
    setItems(prev => {
      const had = new Set(prev.filter(x => x.st !== 'out').map(x => x.m.id));
      const now = want.map((m, i) => ({
        m,
        i,
        st: had.has(m.id) ? 'idle' : 'in'
      }));
      const leaving = prev.filter(x => !ids.has(x.m.id)).map(x => ({
        ...x,
        st: 'out'
      }));
      return [...now, ...leaving];
    });
    let r2 = 0;
    const r1 = requestAnimationFrame(() => {
      r2 = requestAnimationFrame(() => setItems(p => p.map(x => x.st === 'in' ? {
        ...x,
        st: 'idle'
      } : x)));
    });
    const t = setTimeout(() => setItems(p => p.filter(x => x.st !== 'out')), 750);
    return () => {
      cancelAnimationFrame(r1);
      cancelAnimationFrame(r2);
      clearTimeout(t);
    };
  }, [messages, depth]);
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "ta-mbox",
    style: {
      height,
      ...style
    }
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "ta-mbox__stack"
  }, items.map(x => /*#__PURE__*/React.createElement("div", {
    key: x.m.id,
    className: 'ta-mbox__msg is-' + x.st,
    style: {
      '--i': x.i,
      width,
      left: -width / 2,
      zIndex: 100 - x.i
    }
  }, /*#__PURE__*/React.createElement("span", {
    className: "ta-mbox__row"
  }, /*#__PURE__*/React.createElement("span", null, x.m.type), /*#__PURE__*/React.createElement("span", {
    style: {
      color: 'var(--fg-3)'
    }
  }, "#", x.m.id)), x.m.note && /*#__PURE__*/React.createElement("span", {
    className: "ta-mbox__note"
  }, x.m.note)))));
}
Object.assign(__ds_scope, { MailboxStack });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/memory/MailboxStack.jsx", error: String((e && e.message) || e) }); }

// components/memory/MemoryLayout.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const HATCH = 'repeating-linear-gradient(135deg, var(--border-2) 0 1px, transparent 1px 5px), var(--surface-1)';
const FIELD_KINDS = {
  data: {
    bg: 'var(--surface-1)',
    fg: 'var(--fg-1)',
    legend: 'data'
  },
  hot: {
    bg: 'var(--state-running-bg)',
    fg: 'var(--state-running)',
    legend: 'hot path'
  },
  warm: {
    bg: 'var(--state-waiting-bg)',
    fg: 'var(--state-waiting)',
    legend: 'warm'
  },
  cold: {
    bg: 'var(--surface-2)',
    fg: 'var(--fg-2)',
    legend: 'cold'
  },
  ptr: {
    bg: 'color-mix(in srgb, var(--data-4) 26%, var(--surface-1))',
    fg: 'var(--fg-1)',
    legend: 'pointer'
  },
  atomic: {
    bg: 'var(--state-done-bg)',
    fg: 'var(--state-done)',
    legend: 'atomic · cross-core'
  },
  pad: {
    bg: HATCH,
    fg: 'var(--fg-4)',
    legend: 'padding'
  }
};
const natural = s => [1, 2, 4, 8].includes(s) ? s : s % 8 === 0 ? 8 : 1;
function layoutStruct(fields, {
  packed = false
} = {}) {
  let off = 0,
    maxA = 1;
  const out = [];
  for (const f of fields) {
    const a = packed ? 1 : f.align ?? natural(f.size);
    maxA = Math.max(maxA, a);
    const at = f.offset ?? Math.ceil(off / a) * a;
    if (at > off) out.push({
      kind: 'pad',
      size: at - off,
      offset: off,
      name: ''
    });
    out.push({
      kind: 'data',
      ...f,
      offset: at
    });
    off = at + f.size;
  }
  const size = packed ? off : Math.ceil(off / maxA) * maxA;
  if (size > off) out.push({
    kind: 'pad',
    size: size - off,
    offset: off,
    name: '',
    tail: true
  });
  return {
    fields: out,
    size
  };
}
const lbl = {
  font: 'var(--type-label)',
  letterSpacing: 'var(--tracking-label)',
  textTransform: 'uppercase'
};
function MemoryLayout({
  name,
  fields = [],
  packed = false,
  lineSize = 64,
  bytesPerRow,
  rowHeight = 44,
  table = false,
  summary = true,
  legend = true,
  style,
  ...rest
}) {
  const bpr = bytesPerRow || Math.min(lineSize, 64);
  const {
    fields: laid,
    size
  } = layoutStruct(fields, {
    packed
  });
  const rows = Math.max(1, Math.ceil(size / bpr));
  const straddles = f => f.kind !== 'pad' && f.size <= lineSize && Math.floor(f.offset / lineSize) !== Math.floor((f.offset + f.size - 1) / lineSize);
  const segs = [];
  laid.forEach((f, fi) => {
    let s = f.offset;
    const e = f.offset + f.size;
    while (s < e) {
      const r = Math.floor(s / bpr),
        se = Math.min(e, (r + 1) * bpr);
      segs.push({
        f,
        fi,
        r,
        col: s % bpr,
        span: se - s,
        first: s === f.offset,
        last: se === e
      });
      s = se;
    }
  });
  const padBytes = laid.filter(f => f.kind === 'pad').reduce((a, f) => a + f.size, 0);
  const lines = Math.ceil(size / lineSize);
  const bad = laid.filter(straddles);
  const kinds = [...new Set(laid.map(f => f.kind))].filter(k => FIELD_KINDS[k] && k !== 'data' && k !== 'pad');
  const tick = bpr >= 32 ? 8 : 4;
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "ta-mem",
    style: {
      minWidth: 0,
      color: 'var(--fg-1)',
      ...style
    }
  }, rest), name && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'baseline',
      gap: 10,
      marginBottom: 8
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      font: '500 13px var(--font-mono)',
      whiteSpace: 'pre',
      flex: 'none'
    }
  }, "struct ", name)), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'grid',
      gridTemplateColumns: '44px minmax(0,1fr)',
      rowGap: 6
    }
  }, /*#__PURE__*/React.createElement("span", null), /*#__PURE__*/React.createElement("div", {
    style: {
      position: 'relative',
      height: 12
    }
  }, Array.from({
    length: bpr / tick
  }, (_, i) => /*#__PURE__*/React.createElement("span", {
    key: i,
    style: {
      position: 'absolute',
      left: `${i * tick / bpr * 100}%`,
      bottom: 0,
      font: '400 10px/1 var(--font-mono)',
      color: 'var(--fg-4)',
      paddingLeft: 2,
      borderLeft: '1px solid var(--border-2)',
      height: 12,
      display: 'flex',
      alignItems: 'flex-end'
    }
  }, i * tick))), Array.from({
    length: rows
  }, (_, r) => {
    const lineStart = r * bpr % lineSize === 0;
    return /*#__PURE__*/React.createElement(React.Fragment, {
      key: r
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        flexDirection: 'column',
        justifyContent: 'center',
        gap: 3,
        font: '400 10px/1 var(--font-mono)',
        color: 'var(--fg-3)'
      }
    }, /*#__PURE__*/React.createElement("span", null, "+", r * bpr)), /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'grid',
        gridTemplateColumns: `repeat(${bpr}, minmax(0,1fr))`,
        height: rowHeight,
        border: '1.25px solid var(--fg-1)',
        background: `repeating-linear-gradient(to right, transparent 0 calc(100% / ${bpr} - 1px), var(--border-1) calc(100% / ${bpr} - 1px) calc(100% / ${bpr}))`,
        marginTop: lineStart && r > 0 && bpr < lineSize ? 6 : 0
      }
    }, segs.filter(s => s.r === r).map((s, i) => {
      const k = FIELD_KINDS[s.f.kind] || FIELD_KINDS.data;
      const warn = s.f.warn || straddles(s.f);
      const isPad = s.f.kind === 'pad';
      return /*#__PURE__*/React.createElement("div", {
        key: i,
        title: isPad ? `${s.f.size} B padding @ +${s.f.offset}` : `${s.f.type ? s.f.type + ' ' : ''}${s.f.name} · ${s.f.size} B @ +${s.f.offset}${s.f.note ? ' — ' + s.f.note : ''}`,
        style: {
          gridColumn: `${s.col + 1} / span ${s.span}`,
          minWidth: 0,
          background: k.bg,
          boxShadow: `${s.last ? 'inset -1px 0 0 var(--fg-1)' : 'inset 0 0 0 0 transparent'}${warn ? ', inset 0 0 0 1.5px var(--state-error)' : ''}`,
          padding: '5px 6px',
          display: 'flex',
          flexDirection: 'column',
          justifyContent: 'center',
          gap: 3,
          overflow: 'hidden'
        }
      }, !isPad && /*#__PURE__*/React.createElement("span", {
        style: {
          font: '500 11px/1 var(--font-mono)',
          color: warn ? 'var(--state-error)' : s.first ? k.fg : 'var(--fg-3)',
          whiteSpace: 'nowrap',
          overflow: 'hidden',
          textOverflow: 'ellipsis'
        }
      }, s.f.name));
    })));
  })), summary && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      flexWrap: 'wrap',
      gap: '4px 14px',
      marginTop: 10,
      paddingLeft: 44,
      font: '400 11px/1.3 var(--font-mono)',
      color: 'var(--fg-2)'
    }
  }, /*#__PURE__*/React.createElement("span", null, /*#__PURE__*/React.createElement("span", {
    style: {
      color: 'var(--fg-1)',
      fontWeight: 500
    }
  }, "sizeof = ", size, " B")), bad.length > 0 && /*#__PURE__*/React.createElement("span", {
    style: {
      color: 'var(--state-error)'
    }
  }, bad.map(f => f.name).join(', '), " straddle", bad.length === 1 ? 's' : '', " a line boundary")), legend && kinds.length > 0 && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      flexWrap: 'wrap',
      gap: '6px 14px',
      marginTop: 8,
      paddingLeft: 44,
      font: '400 11px/1 var(--font-mono)',
      color: 'var(--fg-3)'
    }
  }, kinds.map(kk => /*#__PURE__*/React.createElement("span", {
    key: kk,
    style: {
      display: 'inline-flex',
      alignItems: 'center',
      gap: 6,
      whiteSpace: 'nowrap'
    }
  }, /*#__PURE__*/React.createElement("i", {
    style: {
      width: 12,
      height: 12,
      background: FIELD_KINDS[kk].bg,
      border: '1px solid var(--fg-1)'
    }
  }), FIELD_KINDS[kk].legend))), table && /*#__PURE__*/React.createElement("div", {
    style: {
      marginTop: 12,
      marginLeft: 44,
      border: '1px solid var(--border-1)',
      font: '400 12px/1 var(--font-mono)'
    }
  }, laid.map((f, i) => /*#__PURE__*/React.createElement("div", {
    key: i,
    style: {
      display: 'grid',
      gridTemplateColumns: '56px 48px minmax(0,1.2fr) minmax(0,1fr) minmax(0,1.4fr)',
      gap: 10,
      alignItems: 'center',
      height: 26,
      padding: '0 10px',
      borderTop: i ? '1px solid var(--border-1)' : 0,
      color: f.kind === 'pad' ? 'var(--fg-4)' : 'var(--fg-1)',
      background: f.kind === 'pad' ? HATCH : undefined
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      color: 'var(--fg-3)'
    }
  }, "+", f.offset), /*#__PURE__*/React.createElement("span", {
    style: {
      color: 'var(--fg-3)'
    }
  }, f.size, " B"), f.kind === 'pad' ? /*#__PURE__*/React.createElement("span", {
    style: {
      gridColumn: '3 / span 3'
    }
  }) : /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("span", {
    style: {
      color: 'var(--fg-2)',
      overflow: 'hidden',
      textOverflow: 'ellipsis',
      whiteSpace: 'nowrap'
    }
  }, f.type), /*#__PURE__*/React.createElement("span", {
    style: {
      fontWeight: 500,
      color: straddles(f) || f.warn ? 'var(--state-error)' : undefined,
      overflow: 'hidden',
      textOverflow: 'ellipsis',
      whiteSpace: 'nowrap'
    }
  }, f.name), /*#__PURE__*/React.createElement("span", {
    style: {
      font: '400 12px/1.2 var(--font-sans)',
      color: 'var(--fg-3)',
      overflow: 'hidden',
      textOverflow: 'ellipsis',
      whiteSpace: 'nowrap'
    }
  }, f.note))))));
}
Object.assign(__ds_scope, { FIELD_KINDS, layoutStruct, MemoryLayout });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/memory/MemoryLayout.jsx", error: String((e && e.message) || e) }); }

// components/memory/ExplodedLayout.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
function ExplodedLayout({
  layers = [],
  exploded,
  defaultExploded = true,
  gap = 44,
  width = 240,
  height = 120,
  stageHeight = 300,
  style,
  ...rest
}) {
  const [open, setOpen] = React.useState(defaultExploded);
  const ex = exploded ?? open;
  const n = layers.length;
  const g = ex ? gap : 4;
  return /*#__PURE__*/React.createElement("div", _extends({
    className: 'ta-explode' + (ex ? ' is-open' : ''),
    onClick: exploded == null ? () => setOpen(o => !o) : undefined,
    style: {
      height: stageHeight,
      ...style
    }
  }, rest), /*#__PURE__*/React.createElement("div", {
    className: "ta-explode__stage",
    style: {
      width,
      height,
      transform: `rotateX(58deg) rotateZ(-36deg) translateZ(${-((n - 1) * g) / 2}px)`
    }
  }, /*#__PURE__*/React.createElement("div", {
    className: "ta-explode__shadow"
  }), layers.map((l, i) => {
    const total = l.fields.reduce((a, f) => a + f.size, 0) || 1;
    return /*#__PURE__*/React.createElement("div", {
      key: i,
      className: "ta-explode__layer",
      style: {
        transform: `translateZ(${(n - 1 - i) * g}px)`
      }
    }, l.fields.map((f, j) => {
      const k = __ds_scope.FIELD_KINDS[f.kind] || __ds_scope.FIELD_KINDS.data;
      return /*#__PURE__*/React.createElement("i", {
        key: j,
        title: `${f.name || 'padding'} · ${f.size} B`,
        style: {
          flex: f.size,
          background: k.bg,
          color: k.fg
        }
      }, f.size / total >= 0.18 ? f.name : '');
    }));
  })), layers.map((l, i) => /*#__PURE__*/React.createElement("span", {
    key: i,
    className: "ta-explode__lead",
    style: {
      top: `calc(50% + ${(((n - 1) / 2 - (n - 1 - i)) * gap * 0.85).toFixed(1)}px)`
    }
  }, l.name)));
}
Object.assign(__ds_scope, { ExplodedLayout });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/memory/ExplodedLayout.jsx", error: String((e && e.message) || e) }); }

// components/memory/RingBuffer.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const lbl = {
  font: 'var(--type-label)',
  letterSpacing: 'var(--tracking-label)',
  textTransform: 'uppercase'
};
function RingBuffer({
  name,
  capacity = 8,
  head = 0,
  tail = 0,
  full = false,
  items = [],
  slotSize,
  producer,
  consumer,
  mode,
  style,
  ...rest
}) {
  const n = full ? capacity : (tail - head + capacity) % capacity;
  const slots = Array.from({
    length: capacity
  }, (_, i) => {
    const k = (i - head + capacity) % capacity;
    return {
      i,
      used: k < n,
      item: k < n ? items[k] : undefined
    };
  });
  const ptr = (label, who, color) => /*#__PURE__*/React.createElement("span", {
    style: {
      display: 'flex',
      flexDirection: 'column',
      alignItems: 'center',
      gap: 3,
      font: '500 11px/1 var(--font-mono)',
      color,
      whiteSpace: 'nowrap'
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      width: 0,
      height: 0,
      borderLeft: '5px solid transparent',
      borderRight: '5px solid transparent',
      borderBottom: `6px solid ${color}`
    }
  }), label, who && /*#__PURE__*/React.createElement("span", {
    style: {
      font: '400 10px/1 var(--font-mono)',
      color: 'var(--fg-3)'
    }
  }, who));
  return /*#__PURE__*/React.createElement("div", _extends({
    className: "ta-ring",
    style: {
      minWidth: 0,
      color: 'var(--fg-1)',
      ...style
    }
  }, rest), /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'baseline',
      gap: 12,
      marginBottom: 8,
      flexWrap: 'wrap'
    }
  }, name && /*#__PURE__*/React.createElement("span", {
    style: {
      font: '500 13px var(--font-mono)'
    }
  }, name), mode && /*#__PURE__*/React.createElement("span", {
    style: {
      ...lbl,
      color: 'var(--fg-4)'
    }
  }, mode), slotSize && /*#__PURE__*/React.createElement("span", {
    style: {
      font: '400 11px var(--font-mono)',
      color: 'var(--fg-3)',
      marginLeft: 'auto'
    }
  }, slotSize, "/slot")), /*#__PURE__*/React.createElement("div", {
    style: {
      position: 'relative',
      display: 'grid',
      gridTemplateColumns: `repeat(${capacity}, minmax(0,1fr))`,
      columnGap: 4,
      rowGap: 6,
      paddingBottom: 22
    }
  }, slots.map(s => /*#__PURE__*/React.createElement("span", {
    key: 'i' + s.i,
    style: {
      gridRow: 1,
      gridColumn: s.i + 1,
      font: '400 10px/1 var(--font-mono)',
      color: 'var(--fg-4)',
      textAlign: 'center'
    }
  }, s.i === 0 || s.i === capacity - 1 ? s.i : '')), slots.map(s => {
    const next = !full && s.i === tail;
    return /*#__PURE__*/React.createElement("div", {
      key: 's' + s.i,
      title: s.used ? String(s.item ?? '') : next ? 'next write' : 'free',
      style: {
        gridRow: 2,
        gridColumn: s.i + 1,
        height: 44,
        minWidth: 0,
        boxSizing: 'border-box',
        border: next ? '1.25px dashed var(--accent)' : '1.25px solid var(--fg-1)',
        background: s.used ? 'var(--state-waiting-bg)' : 'var(--surface-1)',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        padding: '0 4px',
        overflow: 'hidden'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 11px/1 var(--font-mono)',
        color: s.used ? 'var(--fg-1)' : next ? 'var(--accent-fg)' : 'var(--fg-4)',
        whiteSpace: 'nowrap',
        overflow: 'hidden',
        textOverflow: 'ellipsis'
      }
    }, s.used ? s.item ?? '●' : ''));
  }), head === tail ? /*#__PURE__*/React.createElement("div", {
    style: {
      gridRow: 3,
      gridColumn: head + 1,
      justifySelf: 'center'
    }
  }, ptr('head · tail', null, full ? 'var(--fg-1)' : 'var(--accent-fg)')) : /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("div", {
    style: {
      gridRow: 3,
      gridColumn: head + 1,
      justifySelf: 'center'
    }
  }, ptr('head', consumer, 'var(--fg-1)')), /*#__PURE__*/React.createElement("div", {
    style: {
      gridRow: 3,
      gridColumn: tail + 1,
      justifySelf: 'center'
    }
  }, ptr('tail', producer, 'var(--accent-fg)'))), /*#__PURE__*/React.createElement("div", {
    "aria-hidden": "true",
    style: {
      position: 'absolute',
      left: `calc(100% / ${capacity} / 2)`,
      right: `calc(100% / ${capacity} / 2)`,
      bottom: 4,
      height: 10,
      border: '1px solid var(--fg-3)',
      borderTop: 0
    }
  }, /*#__PURE__*/React.createElement("span", {
    style: {
      position: 'absolute',
      left: -4.5,
      top: -4,
      width: 0,
      height: 0,
      borderLeft: '4px solid transparent',
      borderRight: '4px solid transparent',
      borderBottom: '5px solid var(--fg-3)'
    }
  }))));
}
Object.assign(__ds_scope, { RingBuffer });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/memory/RingBuffer.jsx", error: String((e && e.message) || e) }); }

// components/navigation/Tabs.jsx
try { (() => {
function Tabs({
  items = [],
  value,
  defaultValue,
  onChange,
  variant = 'underline',
  style
}) {
  const [inner, setInner] = React.useState(defaultValue ?? (items[0] && (items[0].value ?? items[0])));
  const cur = value ?? inner;
  const pick = v => {
    setInner(v);
    onChange && onChange(v);
  };
  return /*#__PURE__*/React.createElement("div", {
    role: "tablist",
    className: 'ta-tabs' + (variant === 'segmented' ? ' ta-tabs--segmented' : ''),
    style: style
  }, items.map(it => {
    const v = it.value ?? it,
      l = it.label ?? it;
    return /*#__PURE__*/React.createElement("button", {
      key: v,
      role: "tab",
      "aria-selected": cur === v,
      className: "ta-tab",
      onClick: () => pick(v)
    }, l, it.count != null && /*#__PURE__*/React.createElement("span", {
      className: "ta-tab__count"
    }, it.count));
  }));
}
Object.assign(__ds_scope, { Tabs });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/navigation/Tabs.jsx", error: String((e && e.message) || e) }); }

// components/observability/SpanTree.jsx
try { (() => {
function _extends() { return _extends = Object.assign ? Object.assign.bind() : function (n) { for (var e = 1; e < arguments.length; e++) { var t = arguments[e]; for (var r in t) ({}).hasOwnProperty.call(t, r) && (n[r] = t[r]); } return n; }, _extends.apply(null, arguments); }
const PALETTE = ['var(--data-1)', 'var(--data-2)', 'var(--data-3)', 'var(--data-4)', 'var(--data-5)', 'var(--data-6)'];
const hash = s => {
  let h = 0;
  for (let i = 0; i < s.length; i++) h = h * 31 + s.charCodeAt(i) | 0;
  return Math.abs(h);
};
function formatDuration(ms) {
  if (ms < 1) return Math.round(ms * 1000) + 'µs';
  if (ms < 10) return ms.toFixed(2) + 'ms';
  if (ms < 1000) return ms.toFixed(ms < 100 ? 1 : 0) + 'ms';
  return (ms / 1000).toFixed(ms < 10000 ? 2 : 1) + 's';
}
function buildTree(spans) {
  const byId = new Map(spans.map(s => [s.spanId, {
    ...s,
    children: []
  }]));
  const roots = [];
  for (const n of byId.values()) {
    const p = n.parentId && byId.get(n.parentId);
    (p ? p.children : roots).push(n);
  }
  const sort = a => {
    a.sort((x, y) => x.start - y.start);
    a.forEach(n => sort(n.children));
  };
  sort(roots);
  return roots;
}
function SpanTree({
  spans = [],
  selectedId,
  onSelect,
  defaultCollapsed = [],
  serviceColors = {},
  treeWidth = 320,
  rowHeight = 26,
  ticks = 4,
  showRuler = true,
  style,
  ...rest
}) {
  const [collapsed, setCollapsed] = React.useState(() => new Set(defaultCollapsed));
  const roots = React.useMemo(() => buildTree(spans), [spans]);
  const t0 = spans.length ? Math.min(...spans.map(s => s.start)) : 0;
  const t1 = spans.length ? Math.max(...spans.map(s => s.start + s.duration)) : 1;
  const total = Math.max(t1 - t0, 1e-6);
  const color = svc => serviceColors[svc] || PALETTE[hash(svc || '') % PALETTE.length];
  const toggle = id => setCollapsed(c => {
    const n = new Set(c);
    n.has(id) ? n.delete(id) : n.add(id);
    return n;
  });
  const rows = [];
  const walk = (nodes, depth, guides) => nodes.forEach((n, i) => {
    const last = i === nodes.length - 1;
    rows.push({
      n,
      depth,
      guides,
      last
    });
    if (n.children.length && !collapsed.has(n.spanId)) walk(n.children, depth + 1, [...guides, !last]);
  });
  walk(roots, 0, []);
  const grid = `repeating-linear-gradient(to right, var(--border-1) 0 1px, transparent 1px ${100 / ticks}%)`;
  const INDENT = 14;
  return /*#__PURE__*/React.createElement("div", _extends({
    role: "tree",
    className: "ta-spantree",
    style: {
      font: '400 12px/1 var(--font-mono)',
      color: 'var(--fg-1)',
      background: 'var(--surface-1)',
      border: '1px solid var(--border-1)',
      borderRadius: 'var(--radius-2)',
      overflow: 'hidden',
      minWidth: 0,
      ...style
    }
  }, rest), showRuler && /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'grid',
      gridTemplateColumns: `${treeWidth}px minmax(0,1fr)`,
      height: 28,
      borderBottom: '1px solid var(--border-1)',
      background: 'var(--surface-2)',
      color: 'var(--fg-4)',
      font: 'var(--type-label)',
      letterSpacing: 'var(--tracking-label)',
      textTransform: 'uppercase'
    }
  }, /*#__PURE__*/React.createElement("div", {
    style: {
      display: 'flex',
      alignItems: 'center',
      padding: '0 12px',
      borderRight: '1px solid var(--border-1)'
    }
  }), /*#__PURE__*/React.createElement("div", {
    style: {
      position: 'relative',
      margin: '0 56px 0 8px'
    }
  }, Array.from({
    length: ticks + 1
  }, (_, i) => /*#__PURE__*/React.createElement("span", {
    key: i,
    style: {
      position: 'absolute',
      top: 0,
      bottom: 0,
      left: `${i / ticks * 100}%`,
      display: 'flex',
      alignItems: 'center',
      paddingLeft: 4,
      borderLeft: '1px solid var(--border-2)',
      whiteSpace: 'nowrap',
      textTransform: 'none'
    }
  }, formatDuration(total * i / ticks))))), rows.map(({
    n,
    depth,
    guides,
    last
  }) => {
    const sel = n.spanId === selectedId;
    const err = n.status === 'error';
    const c = err ? 'var(--state-error)' : color(n.service);
    const left = (n.start - t0) / total * 100;
    const width = n.duration / total * 100;
    const hasKids = n.children.length > 0;
    const isCollapsed = collapsed.has(n.spanId);
    return /*#__PURE__*/React.createElement("div", {
      key: n.spanId,
      role: "treeitem",
      "aria-selected": sel,
      "aria-expanded": hasKids ? !isCollapsed : undefined,
      "aria-level": depth + 1,
      tabIndex: 0,
      className: 'ta-spantree__row' + (sel ? ' is-selected' : ''),
      onClick: () => onSelect && onSelect(n.spanId, n),
      onKeyDown: e => {
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          onSelect && onSelect(n.spanId, n);
        }
        if (hasKids && (e.key === 'ArrowLeft' && !isCollapsed || e.key === 'ArrowRight' && isCollapsed)) {
          e.preventDefault();
          toggle(n.spanId);
        }
      },
      style: {
        display: 'grid',
        gridTemplateColumns: `${treeWidth}px minmax(0,1fr)`,
        height: rowHeight,
        cursor: 'pointer',
        background: sel ? 'var(--accent-subtle)' : undefined,
        boxShadow: sel ? 'inset 2px 0 0 var(--accent)' : undefined
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        alignItems: 'center',
        minWidth: 0,
        paddingLeft: 6,
        paddingRight: 12,
        borderRight: '1px solid var(--border-1)'
      }
    }, guides.map((g, i) => /*#__PURE__*/React.createElement("span", {
      key: i,
      style: {
        flex: 'none',
        width: INDENT,
        alignSelf: 'stretch',
        background: g ? 'linear-gradient(var(--border-2),var(--border-2)) 6px 0/1px 100% no-repeat' : undefined
      }
    })), depth > 0 && /*#__PURE__*/React.createElement("span", {
      style: {
        flex: 'none',
        width: 0,
        alignSelf: 'stretch',
        position: 'relative'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        position: 'absolute',
        left: -INDENT + 6,
        top: 0,
        height: last ? '50%' : '100%',
        width: 1,
        background: 'var(--border-2)'
      }
    }), /*#__PURE__*/React.createElement("span", {
      style: {
        position: 'absolute',
        left: -INDENT + 6,
        top: '50%',
        width: 6,
        height: 1,
        background: 'var(--border-2)'
      }
    })), /*#__PURE__*/React.createElement("span", {
      onClick: hasKids ? e => {
        e.stopPropagation();
        toggle(n.spanId);
      } : undefined,
      style: {
        flex: 'none',
        width: 16,
        height: 16,
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'center',
        color: 'var(--fg-3)',
        borderRadius: 'var(--radius-1)'
      }
    }, hasKids && /*#__PURE__*/React.createElement(__ds_scope.Icon, {
      name: "chevron-right",
      size: 12,
      style: {
        transform: isCollapsed ? 'none' : 'rotate(90deg)',
        transition: 'transform var(--dur-fast) var(--ease-step)'
      }
    })), /*#__PURE__*/React.createElement("span", {
      style: {
        flex: 'none',
        width: 3,
        height: 14,
        marginRight: 8,
        borderRadius: 1,
        background: c
      }
    }), /*#__PURE__*/React.createElement("span", {
      style: {
        flex: 'none',
        color: 'var(--fg-3)',
        marginRight: 6
      }
    }, n.service), /*#__PURE__*/React.createElement("span", {
      style: {
        minWidth: 0,
        overflow: 'hidden',
        textOverflow: 'ellipsis',
        whiteSpace: 'nowrap',
        color: err ? 'var(--state-error)' : 'var(--fg-1)',
        fontWeight: sel ? 500 : 400
      }
    }, n.name), err && /*#__PURE__*/React.createElement(__ds_scope.Icon, {
      name: "alert-triangle",
      size: 12,
      style: {
        flex: 'none',
        marginLeft: 6,
        color: 'var(--state-error)'
      }
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        position: 'relative',
        margin: '0 56px 0 8px',
        backgroundImage: grid
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        position: 'absolute',
        top: '50%',
        left: `${left}%`,
        width: `max(${width}%, 2px)`,
        height: 10,
        transform: 'translateY(-50%)',
        background: c,
        borderRadius: 1,
        opacity: sel ? 1 : 0.85
      }
    }, (n.events || []).map((ev, i) => /*#__PURE__*/React.createElement("span", {
      key: i,
      title: ev.name,
      style: {
        position: 'absolute',
        top: -2,
        bottom: -2,
        width: 1,
        left: `${n.duration ? (ev.time - n.start) / n.duration * 100 : 0}%`,
        background: 'var(--fg-1)'
      }
    }))), /*#__PURE__*/React.createElement("span", {
      className: 'ta-spantree__dur' + (depth === 0 || err ? ' is-shown' : ''),
      style: {
        position: 'absolute',
        top: '50%',
        transform: 'translateY(-50%)',
        whiteSpace: 'nowrap',
        color: 'var(--fg-3)',
        fontSize: 11,
        left: `calc(${left + width}% + 6px)`
      }
    }, formatDuration(n.duration))));
  }));
}
Object.assign(__ds_scope, { formatDuration, SpanTree });
})(); } catch (e) { __ds_ns.__errors.push({ path: "components/observability/SpanTree.jsx", error: String((e && e.message) || e) }); }

// explorations/depth-sketches.js
try { (() => {
(() => {
  const $ = s => document.querySelector(s);
  const h = (tag, cls, html) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html != null) e.innerHTML = html;
    return e;
  };
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) document.body.classList.add('rm');
  let paused = false;
  const loops = [];
  const every = (ms, fn) => {
    loops.push({
      ms,
      fn,
      t: 0
    });
  };
  setInterval(() => {
    if (paused) return;
    loops.forEach(l => {
      l.t += 100;
      if (l.t >= l.ms) {
        l.t = 0;
        l.fn();
      }
    });
  }, 100);

  /* 1 vellum */
  (() => {
    const root = $('#v1');
    const base = h('div', 'base');
    const row = h('div', 'row8');
    const cells = [];
    for (let i = 0; i < 8; i++) {
      const c = h('div', 'cell');
      row.appendChild(c);
      cells.push(c);
    }
    base.appendChild(row);
    root.appendChild(base);
    const sheet = h('div', 'sheet');
    const span = h('div', 'span');
    const w = h('div', 'hd w', '<span>W</span><span class="tri"></span>'),
      r = h('div', 'hd r', '<span class="tri"></span><span>R</span>');
    sheet.append(span, w, r);
    root.appendChild(sheet);
    let wi = 5,
      ri = 1;
    const X = i => 14 + 24 + i * 44;
    const draw = () => {
      cells.forEach((c, i) => {
        const used = (i - ri + 8) % 8 < (wi - ri + 8) % 8;
        c.className = 'cell' + (used ? ' u' : '');
        c.textContent = used ? 'm' + i : '';
      });
      w.style.transform = `translateX(${X(wi) - 10}px)`;
      r.style.transform = `translateX(${X(ri) - 10}px)`;
      const n = (wi - ri + 8) % 8;
      if (ri + n <= 8) {
        span.style.display = '';
        span.style.left = X(ri) - 12 + 'px';
        span.style.width = n * 44 + 'px';
      } else {
        span.style.display = 'none';
      }
    };
    draw();
    every(1100, () => {
      if (Math.random() < .55) wi = (wi + 1) % 8;else if (ri !== wi) ri = (ri + 1) % 8;
      if ((wi + 1) % 8 === ri) ri = (ri + 1) % 8;
      draw();
    });
  })();

  /* 2 registration */
  (() => {
    const root = $('#v2');
    const gens = [['gen 0', 'crashed', 'var(--state-error)', [40, 30, 60]], ['gen 1', 'crashed', 'var(--state-error)', [40, 30, 70, 20]], ['gen 2', 'paying', 'var(--state-running)', [40, 30, 70, 36, 24]]];
    gens.forEach((g, i) => {
      const d = h('div', 'reg' + (i < 2 ? ' old' : ''), `<div class="nm">checkout.1</div><div class="st"><i style="background:${g[2]}"></i>${g[1]}</div><span class="gen">${g[0]}</span><div class="bars">${g[3].map(w => `<span style="width:${w}px"></span>`).join('')}</div>`);
      d.style.setProperty('--i', 2 - i);
      root.appendChild(d);
    });
  })();

  /* 4 hierarchy */
  (() => {
    const root = $('#v4');
    const T = [['register', '0.3 ns', .3], ['L1', '1 ns', 1], ['L2', '4 ns', 4], ['L3', '15 ns', 15], ['RAM', '100 ns', 100], ['disk', '100 µs', 1e5]];
    const rows = T.map((t, i) => {
      const s = 1 - i * .07,
        o = 1 - i * .13;
      const r = h('div', 'tier', `<span class="nm">${t[0]}</span><span class="tk"><span class="dot"></span></span><span class="lt">${t[1]}</span>`);
      r.style.transform = `scale(${s})`;
      r.style.opacity = o;
      r.style.color = `color-mix(in oklch,var(--fg-1) ${100 - i * 12}%,var(--bg-app))`;
      root.appendChild(r);
      return r;
    });
    let k = 0;
    every(1600, () => {
      const tgt = [1, 2, 4, 3, 1, 5][k++ % 6];
      rows.forEach((r, i) => {
        const d = r.querySelector('.dot');
        d.style.transition = 'none';
        d.style.opacity = 0;
        d.style.left = '-4px';
      });
      const d = rows[tgt].querySelector('.dot');
      const dur = 200 + Math.log10(T[tgt][2] * 10) * 180;
      d.offsetWidth;
      d.style.transition = `left ${dur}ms var(--e-move),opacity 150ms`;
      d.style.opacity = 1;
      d.style.left = 'calc(100% - 4px)';
      rows.forEach((r, i) => {
        r.style.opacity = i === tgt ? 1 : Math.max(.25, 1 - i * .13);
      });
      setTimeout(() => {
        d.style.opacity = 0;
        rows.forEach((r, i) => r.style.opacity = 1 - i * .13);
      }, dur + 250);
    });
  })();

  /* 5 depth of field */
  (() => {
    const root = $('#v5');
    const A = [['gateway', 'running', 'var(--state-running)'], ['auth', 'waiting', 'var(--state-waiting)'], ['checkout.1', 'running', 'var(--state-running)'], ['checkout.2', 'done', 'var(--state-done)'], ['ledger', 'running', 'var(--state-running)'], ['mailer', 'stopped', 'var(--state-stopped)'], ['indexer', 'waiting', 'var(--state-waiting)'], ['billing', 'running', 'var(--state-running)']];
    const els = A.map(a => {
      const e = h('div', 'act', `<span class="nm">${a[0]}</span><span class="st"><i></i>${a[1]}</span>`);
      e.style.setProperty('--c', a[2]);
      root.appendChild(e);
      return e;
    });
    const path = [0, 1, 2, 4, 7, 2, 3];
    let k = 0;
    const go = () => {
      const f = path[k++ % path.length];
      els.forEach((e, i) => {
        e.classList.toggle('foc', i === f);
        e.classList.toggle('blur', i !== f);
      });
    };
    go();
    every(1800, go);
    root.addEventListener('mouseover', e => {
      const a = e.target.closest('.act');
      if (!a) return;
      els.forEach(x => {
        x.classList.toggle('foc', x === a);
        x.classList.toggle('blur', x !== a);
      });
    });
  })();

  /* 6 lift */
  (() => {
    const root = $('#v6');
    const B = [{
      id: 'gw',
      n: 'gateway',
      x: 40,
      y: 40
    }, {
      id: 'rt',
      n: 'router',
      x: 250,
      y: 110
    }, {
      id: 'db',
      n: 'ledger',
      x: 40,
      y: 180
    }, {
      id: 'q',
      n: 'outbox',
      x: 420,
      y: 40
    }];
    const W = [['gw', 'rt'], ['db', 'rt'], ['rt', 'q']];
    const off = {};
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    root.appendChild(svg);
    const port = (b, side) => {
      const o = off[b.id] || [0, 0];
      return side === 'r' ? [b.x + 132 + o[0], b.y + 31 + o[1]] : [b.x + o[0], b.y + 31 + o[1]];
    };
    const paths = W.map(() => {
      const p = document.createElementNS('http://www.w3.org/2000/svg', 'path');
      svg.appendChild(p);
      return p;
    });
    const by = Object.fromEntries(B.map(b => [b.id, b]));
    const pads = [];
    const draw = () => {
      W.forEach(([a, b], i) => {
        const A = port(by[a], 'r'),
          Z = port(by[b], 'l');
        const mx = (A[0] + Z[0]) / 2;
        paths[i].setAttribute('d', `M${A[0]} ${A[1]} L${mx} ${A[1]} L${mx} ${Z[1]} L${Z[0]} ${Z[1]}`);
      });
      pads.forEach(p => {
        const pt = port(by[p.b], p.s);
        p.el.style.left = pt[0] + 'px';
        p.el.style.top = pt[1] + 'px';
      });
    };
    B.forEach(b => {
      const e = h('div', 'blk6', `<span class="nm">${b.n}</span>`);
      e.style.left = b.x + 'px';
      e.style.top = b.y + 'px';
      root.appendChild(e);
      e.onmouseenter = () => {
        e.classList.add('up');
        off[b.id] = [-4, -8];
        draw();
      };
      e.onmouseleave = () => {
        e.classList.remove('up');
        off[b.id] = [0, 0];
        draw();
      };
      ['l', 'r'].forEach(s => {
        if (W.some(([a, z]) => s === 'r' && a === b.id || s === 'l' && z === b.id)) {
          const p = h('span', 'pad6');
          root.appendChild(p);
          pads.push({
            b: b.id,
            s,
            el: p
          });
        }
      });
    });
    draw();
    let k = 0;
    every(2200, () => {
      if (root.matches(':hover')) return;
      const els = [...root.querySelectorAll('.blk6')];
      els.forEach(e => e.classList.remove('up'));
      B.forEach(b => off[b.id] = [0, 0]);
      const i = [1, 0, 3, 2][k++ % 4];
      els[i].classList.add('up');
      off[B[i].id] = [-4, -8];
      draw();
    });
  })();

  /* 7 axo */
  (() => {
    const root = $('#v7');
    const axo = h('div', 'axo');
    root.appendChild(axo);
    const L = [{
      n: 'padding',
      c: 'pad',
      f: [['', 8]]
    }, {
      n: 'payload',
      f: [['flags', 2, 3], ['', 6, 'pad'], ['mailbox', 8, 4]]
    }, {
      n: 'header',
      f: [['state', 1, 1], ['', 7, 'pad'], ['id', 8, 2]]
    }];
    axo.appendChild(Object.assign(h('div', 'ly sh'), {}));
    L.forEach((l, i) => {
      const d = h('div', 'ly');
      d.style.setProperty('--i', i);
      d.style.gridTemplateColumns = l.c === 'pad' ? '1fr' : l.f.map(f => f[1] + 'fr').join(' ');
      if (l.c === 'pad') {
        d.style.background = 'var(--hatch),var(--surface-1)';
        d.innerHTML = '<i style="color:var(--fg-4)">8 B tail</i>';
      } else {
        d.style.background = 'var(--surface-1)';
        d.innerHTML = l.f.map(f => `<i style="background:${f[2] === 'pad' ? 'var(--hatch),var(--surface-1)' : `color-mix(in oklch,var(--data-${f[2]}) 38%,var(--surface-1))`}">${f[0]}</i>`).join('');
      }
      axo.appendChild(d);
    });
    [['header · 16 B', '28%'], ['payload · 16 B', '46%'], ['padding · 8 B', '64%']].forEach(([t, y]) => {
      const e = h('span', 'lead', t);
      e.style.top = y;
      root.appendChild(e);
    });
    let auto = true;
    root.onclick = () => {
      auto = false;
      root.classList.toggle('ex');
    };
    every(3200, () => {
      if (auto) root.classList.toggle('ex');
    });
  })();

  /* 8 stack */
  (() => {
    const root = $('#v8');
    let id = 1;
    const msgs = [];
    const types = ['PAY', 'INC', 'SNAP', 'PING', 'STOP', 'ACK'];
    const mk = () => {
      const t = types[id % types.length];
      const e = h('div', 'msg', `<span class="mono"><span>${t}</span><span style="color:var(--fg-3)">#${id}</span></span><span class="bar" style="width:${30 + id * 37 % 60}%"></span>`);
      id++;
      return e;
    };
    const lay = () => msgs.forEach((m, i) => m.style.setProperty('--i', i));
    for (let i = 0; i < 5; i++) {
      const m = mk();
      msgs.push(m);
    }
    msgs.reverse().forEach(m => root.prepend(m));
    msgs.reverse();
    const order = () => {
      msgs.forEach((m, i) => m.style.zIndex = 100 - i);
    };
    lay();
    order();
    let tick = 0;
    every(1300, () => {
      tick++;
      if (tick % 2) {
        const m = mk();
        m.classList.add('in');
        m.style.setProperty('--i', 0);
        root.appendChild(m);
        msgs.unshift(m);
        order();
        m.offsetWidth;
        m.classList.remove('in');
        lay();
        if (msgs.length > 6) {
          const o = msgs.pop();
          o.classList.add('out');
          setTimeout(() => o.remove(), 700);
        }
      } else if (msgs.length > 3) {
        const o = msgs.pop();
        o.classList.add('out');
        setTimeout(() => o.remove(), 700);
      }
    });
  })();

  /* 9 board */
  (() => {
    const root = $('#v9');
    const B = [{
      n: 'gateway',
      t: 'C · epoll',
      x: 30,
      y: 30,
      w: 120,
      h: 56
    }, {
      n: 'router',
      t: 'actor',
      x: 200,
      y: 90,
      w: 110,
      h: 56
    }, {
      n: 'ring',
      t: 'SPSC · 4 KB',
      x: 200,
      y: 170,
      w: 110,
      h: 40,
      mem: 1
    }, {
      n: 'ledger',
      t: 'actor',
      x: 330,
      y: 20,
      w: 96,
      h: 56
    }];
    const svg = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
    root.appendChild(svg);
    const P = ['M150 58 L175 58 L175 118 L200 118', 'M310 118 L318 118 L318 48 L330 48', 'M255 146 L255 170'];
    P.forEach(d => {
      ['', 'fl'].forEach(c => {
        const p = document.createElementNS('http://www.w3.org/2000/svg', 'path');
        p.setAttribute('d', d);
        if (c) p.setAttribute('class', c);
        svg.appendChild(p);
      });
    });
    B.forEach(b => {
      const e = h('div', 'b9' + (b.mem ? ' mem' : ''), `<div class="s"></div><div class="w"></div><div class="e"></div><div class="t"><span class="mono">${b.n}</span><span class="sm">${b.t}</span></div>`);
      Object.assign(e.style, {
        left: b.x + 'px',
        top: b.y + 'px',
        width: b.w + 'px',
        height: b.h + 'px'
      });
      root.appendChild(e);
    });
  })();

  /* 10 orbit */
  (() => {
    const stage = $('#v10'),
      obj = $('#obj');
    const S = 9,
      H = 46,
      D = 46;
    const F = [['state', 1, 1], ['', 7, 0], ['id', 8, 2], ['flags', 2, 3], ['', 6, 0], ['mailbox', 8, 4], ['refs', 4, 6], ['prio', 1, 5], ['', 3, 0]];
    const total = F.reduce((a, f) => a + f[1], 0);
    let x = -total * S / 2;
    const face = (w, hh, tf, bg, txt) => {
      const i = h('i', '', txt || '');
      Object.assign(i.style, {
        width: w + 'px',
        height: hh + 'px',
        transform: tf,
        background: bg
      });
      return i;
    };
    F.forEach(f => {
      const w = f[1] * S;
      const bg = f[2] ? `color-mix(in oklch,var(--data-${f[2]}) 42%,var(--surface-1))` : 'var(--hatch),var(--surface-1)';
      const bg2 = f[2] ? `color-mix(in oklch,var(--data-${f[2]}) 62%,var(--surface-2))` : 'var(--hatch),var(--surface-2)';
      const c = h('div', 'cub');
      c.style.transform = `translate3d(${x}px,${-H / 2}px,${-D / 2}px)`;
      c.append(face(w, H, `translateZ(${D}px)`, bg, w > 30 ? f[0] : ''), face(w, H, `rotateY(180deg)`, bg), face(D, H, `rotateY(-90deg)`, bg2), face(D, H, `translateX(${w}px) rotateY(90deg) translateX(-${D}px)`.replace('translateX(-', 'translateX(-'), bg2), face(w, D, `rotateX(90deg)`, bg2), face(w, D, `translateY(${H}px) rotateX(-90deg) translateY(-${D}px)`, bg2));
      const ff = c.children;
      ff[2].style.transformOrigin = 'left center';
      ff[2].style.transform = 'rotateY(90deg)';
      ff[3].style.transformOrigin = 'left center';
      ff[3].style.transform = `translateX(${w}px) rotateY(90deg)`;
      ff[4].style.transformOrigin = 'top center';
      ff[4].style.transform = 'rotateX(-90deg)';
      ff[5].style.transformOrigin = 'top center';
      ff[5].style.transform = `translateY(${H}px) rotateX(-90deg)`;
      ff[0].style.transform = `translateZ(${D}px)`;
      ff[1].style.transform = 'none';
      obj.appendChild(c);
      x += w;
    });
    let rx = -24,
      ry = -32,
      drag = null,
      idle = true;
    const set = () => obj.style.transform = `rotateX(${rx}deg) rotateY(${ry}deg)`;
    set();
    stage.addEventListener('pointerdown', e => {
      drag = [e.clientX, e.clientY, rx, ry];
      idle = false;
      stage.setPointerCapture(e.pointerId);
    });
    stage.addEventListener('pointermove', e => {
      if (!drag) return;
      ry = drag[3] + (e.clientX - drag[0]) * .5;
      rx = Math.max(-80, Math.min(80, drag[2] - (e.clientY - drag[1]) * .5));
      set();
    });
    stage.addEventListener('pointerup', () => {
      drag = null;
      setTimeout(() => idle = true, 2500);
    });
    let last = performance.now();
    const spin = t => {
      const dt = t - last;
      last = t;
      if (idle && !paused && !document.body.classList.contains('rm')) {
        ry += dt * .012;
        set();
      }
      requestAnimationFrame(spin);
    };
    requestAnimationFrame(spin);
  })();

  /* 11 parallax */
  (() => {
    const el = $('#v11');
    const upd = () => {
      el.style.backgroundPosition = `0 ${-el.scrollTop * .5}px`;
    };
    el.addEventListener('scroll', upd);
    upd();
    let dir = 1,
      hover = false;
    el.onmouseenter = () => hover = true;
    el.onmouseleave = () => hover = false;
    const step = () => {
      if (!hover && !paused && !document.body.classList.contains('rm')) {
        const max = el.scrollHeight - el.clientHeight;
        el.scrollTop += dir * .5;
        if (el.scrollTop >= max - 1) dir = -1;
        if (el.scrollTop <= 0) dir = 1;
      }
      requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  })();

  /* 12 dive */
  (() => {
    const root = $('#v12');
    const NS = 'http://www.w3.org/2000/svg';
    const crumb = h('div', 'crumb', '<button id="c0">system</button><span id="c1" style="display:none">/ <span style="color:var(--fg-1)">ingest</span></span>');
    root.appendChild(crumb);
    const l0 = h('div', 'dv0'),
      l1 = h('div', 'dv1');
    root.append(l0, l1);
    const blk = (p, b, cls) => {
      const e = h('div', 'b12' + (cls ? ' ' + cls : ''), `<span class="mono">${b.n}</span><span class="sm">${b.t}</span>`);
      Object.assign(e.style, {
        left: b.x + 'px',
        top: b.y + 'px',
        width: b.w + 'px',
        height: b.h + 'px'
      });
      p.appendChild(e);
      return e;
    };
    const wires = (p, ds) => {
      const s = document.createElementNS(NS, 'svg');
      ds.forEach(d => {
        const q = document.createElementNS(NS, 'path');
        q.setAttribute('d', d);
        s.appendChild(q);
      });
      p.prepend(s);
    };
    const sx = x => `calc(50% + ${x}px)`;
    const S = [{
      n: 'clients',
      t: 'external',
      x: -240,
      y: 110,
      w: 110,
      h: 56
    }, {
      n: 'ingest',
      t: 'container · C',
      x: -60,
      y: 110,
      w: 120,
      h: 56
    }, {
      n: 'ledger',
      t: 'container',
      x: 130,
      y: 60,
      w: 110,
      h: 56
    }, {
      n: 'archive',
      t: 'store',
      x: 130,
      y: 170,
      w: 110,
      h: 56
    }];
    const els = S.map(b => {
      const e = blk(l0, {
        ...b,
        x: 0
      });
      e.style.left = sx(b.x);
      return e;
    });
    els[0].style.borderStyle = 'dashed';
    const C = [{
      n: 'accept',
      t: 'epoll loop',
      x: -220,
      y: 70,
      w: 110,
      h: 50
    }, {
      n: 'parse',
      t: 'actor',
      x: -60,
      y: 70,
      w: 110,
      h: 50
    }, {
      n: 'ring',
      t: 'SPSC · 4 KB',
      x: -60,
      y: 170,
      w: 110,
      h: 44
    }, {
      n: 'route',
      t: 'actor',
      x: 110,
      y: 70,
      w: 110,
      h: 50
    }];
    let w = root.getBoundingClientRect().width || 520;
    const cx = x => w / 2 + x;
    const draw = () => {
      l0.querySelector('svg')?.remove();
      l1.querySelector('svg')?.remove();
      w = root.getBoundingClientRect().width || 520;
      wires(l0, [`M${cx(-130)} 138 L${cx(-60)} 138`, `M${cx(60)} 138 L${cx(95)} 138 L${cx(95)} 88 L${cx(130)} 88`, `M${cx(60)} 138 L${cx(95)} 138 L${cx(95)} 198 L${cx(130)} 198`]);
      wires(l1, [`M${cx(-110)} 95 L${cx(-60)} 95`, `M${cx(50)} 95 L${cx(110)} 95`, `M${cx(-5)} 120 L${cx(-5)} 170`]);
    };
    C.forEach(b => {
      const e = blk(l1, {
        ...b,
        x: 0
      }, 'cmp');
      e.style.left = sx(b.x);
    });
    draw();
    new ResizeObserver(draw).observe(root);
    l1.prepend(h('div', 'zone12'));
    const tgt = els[1];
    const dive = () => {
      l0.style.transformOrigin = `${cx(0)}px 138px`;
      l1.style.transformOrigin = `${cx(0)}px 138px`;
      l0.classList.add('away');
      l1.classList.add('here');
      $('#c1').style.display = '';
    };
    const up = () => {
      l0.classList.remove('away');
      l1.classList.remove('here');
      $('#c1').style.display = 'none';
    };
    let auto = true;
    tgt.onclick = () => {
      auto = false;
      dive();
    };
    $('#c0').onclick = () => {
      auto = false;
      up();
    };
    let k = 0;
    every(3400, () => {
      if (!auto) return;
      (k++ % 2 ? up : dive)();
    });
  })();
  $('#bPause').onclick = e => {
    paused = !paused;
    e.target.textContent = paused ? 'Play' : 'Pause';
    document.querySelectorAll('.board path.fl').forEach(p => p.style.animationPlayState = paused ? 'paused' : 'running');
  };
  $('#bTheme').onclick = e => {
    const d = document.documentElement.dataset.theme === 'dark';
    if (d) delete document.documentElement.dataset.theme;else document.documentElement.dataset.theme = 'dark';
    e.target.textContent = d ? 'Dark' : 'Light';
  };
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "explorations/depth-sketches.js", error: String((e && e.message) || e) }); }

// explorations/memory-sketches.js
try { (() => {
(() => {
  const $ = s => document.querySelector(s);
  const h = (tag, cls, html) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (html != null) e.innerHTML = html;
    return e;
  };
  const D = {
    step: 420,
    fill: 480,
    pub: 240,
    ring: 900,
    drain: 360,
    wrap: 700,
    fade: 240
  };
  let K = 1,
    paused = false,
    timer;
  const later = (ms, fn) => setTimeout(fn, ms * K);
  const reflow = e => void e.offsetWidth;
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) document.body.classList.add('rm');
  const W = 'var(--accent-fg)',
    R = 'var(--fg-1)';
  class Tape {
    constructor(el, {
      n,
      sw = 44,
      lanes = [],
      idx,
      wrap = false,
      wrapLabel = 'wrap',
      cls = ''
    }) {
      this.n = n;
      this.sw = sw;
      this.gap = 4;
      this.heads = {};
      this.pos = {};
      const t = h('div', 'tape ' + cls);
      t.style.setProperty('--n', n);
      t.style.setProperty('--sw', sw + 'px');
      const mk = l => {
        l.el = h('div', 'lane' + (l.pos === 'bot' ? ' bot' : '') + (l.sm ? ' sm' : ''));
        return l.el;
      };
      lanes.filter(l => l.pos === 'top').forEach(l => t.appendChild(mk(l)));
      if (idx) {
        const r = h('div', 'idx');
        idx.forEach(s => r.appendChild(h('span', '', s)));
        t.appendChild(r);
      }
      const row = h('div', 'slots');
      this.slots = [];
      for (let i = 0; i < n; i++) {
        const s = h('div', 'slot', '<div class="fill"></div><div class="ring"></div><div class="v"></div>');
        row.appendChild(s);
        this.slots.push(s);
      }
      t.appendChild(row);
      lanes.filter(l => l.pos !== 'top').forEach(l => t.appendChild(mk(l)));
      if (wrap) {
        this.wl = h('div', 'wrapline', '<span>' + wrapLabel + '</span>');
        t.appendChild(this.wl);
      }
      this.lanes = Object.fromEntries(lanes.map(l => [l.key, l.el]));
      el.appendChild(t);
      this.el = t;
    }
    head(id, lane, label, color) {
      const d = h('div', 'head', '<b></b><span>' + label + '</span><span class="hs"></span>');
      d.style.setProperty('--hc', color);
      this.lanes[lane].appendChild(d);
      this.heads[id] = d;
      this.pos[id] = 0;
      return d;
    }
    sub(id, t) {
      this.heads[id].querySelector('.hs').textContent = t;
    }
    move(id, i, jump) {
      const d = this.heads[id],
        prev = this.pos[id],
        X = n => `translateX(${n * (this.sw + this.gap)}px)`;
      this.pos[id] = i;
      if (i < prev || jump) {
        const wrap = i < prev && !jump && !!this.wl;
        if (wrap) {
          d.style.transform = X(prev + 0.6);
          this.flash(d.style.getPropertyValue('--hc'));
        }
        d.style.opacity = 0;
        later(D.fade, () => {
          d.classList.add('nt');
          d.style.transform = X(i - (wrap ? 0.6 : 0));
          reflow(d);
          d.classList.remove('nt');
          d.style.opacity = 1;
          d.style.transform = X(i);
        });
      } else d.style.transform = X(i);
    }
    fadeClear(i) {
      this.set(i, {
        drain: true,
        cur: false
      });
      later(D.drain, () => this.clear(i));
    }
    flash(c) {
      const w = this.wl;
      w.style.setProperty('--hc', c);
      w.classList.remove('pulse');
      reflow(w);
      w.classList.add('pulse');
    }
    set(i, o) {
      const s = this.slots[i];
      if ('val' in o) {
        const v = s.querySelector('.v');
        v.innerHTML = o.val == null ? '' : o.val + (o.sub ? `<small>${o.sub}</small>` : '');
        if (o.val != null && o.on) {
          v.classList.remove('vin');
          reflow(v);
          v.classList.add('vin');
        }
      }
      ['on', 'pending', 'pub', 'drain', 'blocked', 'cur'].forEach(k => {
        if (k in o) s.classList.toggle(k, !!o[k]);
      });
      if (o.pips) {
        let p = s.querySelector('.pips');
        if (!p) {
          p = h('div', 'pips');
          s.appendChild(p);
        }
        p.innerHTML = o.pips.map(x => `<i class="${x ? 'x' : ''}"></i>`).join('');
      }
    }
    clear(i) {
      const s = this.slots[i];
      s.className = 'slot nt';
      s.querySelector('.v').innerHTML = '';
      const p = s.querySelector('.pips');
      if (p) p.remove();
      reflow(s);
      s.classList.remove('nt');
    }
  }
  const tickers = [];
  function start() {
    clearInterval(timer);
    if (!paused) timer = setInterval(() => tickers.forEach(f => f()), 900 * K);
  }

  /* 1a ByteOrder */
  (() => {
    const root = $('#bo');
    const addrs = ['0x1000', '0x1001', '0x1002', '0x1003'];
    const mkSet = (name, order) => {
      const w = h('div');
      w.appendChild(h('div', 'sub', name));
      const m = new Tape(w, {
        n: 4,
        sw: 56,
        idx: addrs,
        lanes: [{
          key: 't',
          pos: 'top'
        }]
      });
      m.head('ld', 't', 'load', W);
      order.forEach((b, i) => m.set(i, {
        val: b,
        on: true
      }));
      w.appendChild(h('div', 'arrow', '↓ register'));
      const r = new Tape(w, {
        n: 4,
        sw: 56,
        idx: ['31–24', '23–16', '15–8', '7–0'],
        cls: 'reg'
      });
      root.appendChild(w);
      return {
        m,
        r,
        order
      };
    };
    const sets = [Object.assign(mkSet('Big-endian · network order', ['0A', '0B', '0C', '0D']), {
      map: i => i
    }), Object.assign(mkSet('Little-endian · x86, ARM default', ['0D', '0C', '0B', '0A']), {
      map: i => 3 - i
    })];
    let s = 0;
    tickers.push(() => {
      sets.forEach(o => {
        if (s < 4) {
          o.m.move('ld', s);
          o.m.slots.forEach((el, i) => o.m.set(i, {
            cur: i === s
          }));
          const ri = o.map(s);
          o.r.set(ri, {
            val: o.order[s],
            on: true
          });
        } else if (s === 7) {
          o.m.slots.forEach((el, i) => o.m.set(i, {
            cur: false
          }));
          for (let i = 0; i < 4; i++) o.r.fadeClear(i);
          o.m.move('ld', 0, true);
        }
      });
      s = (s + 1) % 8;
    });
  })();

  /* 1b WireLayout */
  (() => {
    const root = $('#pb');
    const bytes = ['08', 'AC', '02', '12', '03', '50', '41', '59'];
    const t = new Tape(root, {
      n: 8,
      sw: 46,
      idx: bytes.map((b, i) => String(i)),
      lanes: [{
        key: 't',
        pos: 'top'
      }]
    });
    t.head('d', 't', 'decode', W);
    bytes.forEach((b, i) => t.set(i, {
      val: b
    }));
    const g = h('div', 'groups');
    [[1, 'tag 1'], [2, 'id = 300'], [1, 'tag 2'], [1, 'len 3'], [3, 'type = "PAY"']].forEach(([n, l]) => {
      const s = h('span', '', `<em>${l}</em>`);
      s.style.gridColumn = 'span ' + n;
      g.appendChild(s);
    });
    t.el.appendChild(g);
    const dp = h('div', 'decode');
    root.appendChild(dp);
    const ex = ['<b>08</b> tag · 0x08 = (1 &lt;&lt; 3) | 0 → field 1, wire type 0 (varint)', '<b>AC</b> = 1·0101100 · continuation bit set, keep 0101100 (44)', '<b>02</b> = 0·0000010 · last byte → (2 &lt;&lt; 7) | 44 = <b>300</b> → id', '<b>12</b> tag · 0x12 = (2 &lt;&lt; 3) | 2 → field 2, wire type 2 (length-delimited)', '<b>03</b> length · the next 3 bytes are the payload', '<b>50</b> → "P"', '<b>41</b> → "A"', '<b>59</b> → "Y" · type = <b>"PAY"</b>'];
    let s = 0;
    dp.innerHTML = '<span style="color:var(--fg-3)">8 bytes · press play</span>';
    tickers.push(() => {
      if (s < 8) {
        t.move('d', s);
        bytes.forEach((b, i) => t.set(i, {
          cur: i === s,
          on: i < s
        }));
        dp.innerHTML = ex[s];
      } else if (s === 10) {
        bytes.forEach((b, i) => t.set(i, {
          on: false,
          cur: false
        }));
        t.move('d', 0, true);
        dp.innerHTML = '<span style="color:var(--fg-3)">restart</span>';
      }
      s = (s + 1) % 11;
    });
  })();

  /* shared SPSC ring: 1c, 2a, 2b, 2c */
  const CAP = 8,
    ring = {
      w: 0,
      r: 0,
      pub: 0,
      t: 0,
      blocked: false,
      vals: {}
    };
  const subs = [];
  let mid = 100;
  const emit = e => subs.forEach(f => f(e));
  tickers.push(() => {
    ring.t++;
    const ph = ring.t % 28;
    const wantW = ph < 12 || ph % 5 === 0;
    const wantR = ring.t % 2 === 0;
    if (wantW && ring.w - ring.r >= CAP) {
      ring.blocked = true;
      emit({
        type: 'block'
      });
    } else {
      ring.blocked = false;
      if (wantW) {
        const seq = ring.w++;
        ring.vals[seq] = 'm' + mid++;
        emit({
          type: 'write',
          seq
        });
        later(D.fill, () => {
          ring.pub = Math.max(ring.pub, seq + 1);
          emit({
            type: 'pub',
            seq
          });
        });
      }
    }
    if (wantR && ring.r < ring.pub) {
      const seq = ring.r++;
      emit({
        type: 'read',
        seq
      });
    }
    emit({
      type: 'tick'
    });
  });
  (() => {
    const root = $('#spsc');
    const t = new Tape(root, {
      n: 8,
      idx: [...Array(8)].map((_, i) => String(i)),
      lanes: [{
        key: 'w',
        pos: 'top'
      }, {
        key: 'r',
        pos: 'bot'
      }],
      wrap: true,
      wrapLabel: 'wrap · seq & 7'
    });
    t.head('w', 'w', 'W', W);
    t.head('r', 'r', 'R', R);
    const meta = h('div', 'meta');
    root.appendChild(meta);
    subs.push(e => {
      const sl = e.seq & 7;
      if (e.type === 'write') {
        t.set(sl, {
          val: ring.vals[e.seq],
          on: true,
          pending: true,
          blocked: false
        });
        const nx = ring.w & 7;
        later(D.fill * 0.6, () => t.move('w', nx));
      }
      if (e.type === 'pub') {
        t.set(sl, {
          pending: false,
          pub: true
        });
        later(D.step, () => t.set(sl, {
          pub: false
        }));
      }
      if (e.type === 'read') {
        t.set(sl, {
          drain: true
        });
        later(D.drain, () => t.clear(sl));
        const nr = ring.r & 7;
        later(D.drain * 0.5, () => t.move('r', nr));
      }
      if (e.type === 'tick') {
        t.heads.w.classList.toggle('blocked', ring.blocked);
        t.sub('w', ring.blocked ? 'blocked' : '');
        t.slots.forEach((s, i) => t.set(i, {
          blocked: ring.blocked && i === (ring.w & 7)
        }));
        meta.innerHTML = ring.blocked ? '<span class="warn">writer blocked</span>' : '';
      }
    });
  })();
  (() => {
    const root = $('#dial');
    const cx = 120,
      pt = (r, a) => {
        const t = (a - 90) * Math.PI / 180;
        return [cx + r * Math.cos(t), cx + r * Math.sin(t)];
      };
    let svg = `<svg class="dial" viewBox="-12 -12 264 264">`;
    for (let i = 0; i < 8; i++) {
      const a0 = i * 45 + 1.5,
        a1 = (i + 1) * 45 - 1.5,
        [x0, y0] = pt(100, a0),
        [x1, y1] = pt(100, a1),
        [x2, y2] = pt(62, a1),
        [x3, y3] = pt(62, a0);
      svg += `<path class="sec" data-i="${i}" d="M${x0} ${y0}A100 100 0 0 1 ${x1} ${y1}L${x2} ${y2}A62 62 0 0 0 ${x3} ${y3}Z"/>`;
      const [tx, ty] = pt(112, i * 45 + 22.5),
        [vx, vy] = pt(81, i * 45 + 22.5);
      svg += `<text x="${tx}" y="${ty + 3}" text-anchor="middle">${i}</text><text class="sv" data-v="${i}" x="${vx}" y="${vy + 3}" text-anchor="middle"></text>`;
    }
    svg += `<g class="hd" id="dW"><path d="M120 0 L114 -10 L126 -10Z" fill="var(--accent-fg)" transform="translate(0 20)"/></g><g class="hd" id="dR"><path d="M120 62 L114 72 L126 72Z" fill="var(--fg-1)" transform="translate(0 -4)"/></g>`;
    svg += `<text class="ctr" id="dC" x="120" y="122" text-anchor="middle">0/8</text><text class="ctr2" x="120" y="138" text-anchor="middle">used</text></svg>`;
    const wrap = h('div', 'dwrap', svg + '<div class="meta" style="flex-direction:column;gap:8px"><span style="color:var(--accent-fg)">▼ W outside · next write</span><span>▲ R inside · next read</span><span id="dS"></span></div>');
    root.appendChild(wrap);
    const secs = [...root.querySelectorAll('.sec')],
      vals = [...root.querySelectorAll('.sv')];
    subs.push(e => {
      if (e.type !== 'tick' && e.type !== 'pub') return;
      secs.forEach((p, i) => {
        p.classList.remove('u', 'nx');
      });
      vals.forEach(v => v.textContent = '');
      for (let q = ring.r; q < ring.w; q++) {
        secs[q & 7].classList.add('u');
        vals[q & 7].textContent = ring.vals[q];
      }
      if (!ring.blocked) secs[ring.w & 7].classList.add('nx');
      $('#dW').style.transform = `rotate(${ring.w * 45 + 22.5}deg)`;
      $('#dR').style.transform = `rotate(${ring.r * 45 + 22.5}deg)`;
      $('#dC').textContent = ring.w - ring.r + '/8';
      $('#dS').innerHTML = ring.blocked ? '<span class="warn" style="color:var(--state-waiting)">blocked · W caught R</span>' : '';
    });
  })();
  (() => {
    const root = $('#seq');
    const v = h('div', 'seqv');
    const s = h('div', 'seqs');
    const cells = h('div');
    const map = new Map();
    s.appendChild(cells);
    const win = h('div', 'win');
    win.style.width = 8 * 48 - 4 + 'px';
    s.appendChild(win);
    const hw = h('div', 'sh w', '<span>W</span><b></b>'),
      hr = h('div', 'sh r', '<b></b><span>R</span>');
    s.appendChild(hw);
    s.appendChild(hr);
    v.appendChild(s);
    root.appendChild(v);
    subs.push(e => {
      if (e.type !== 'tick' && e.type !== 'pub') return;
      const lo = Math.max(0, ring.r - 4),
        hi = ring.r + 14;
      for (const [q, el] of map) {
        if (q < lo || q >= hi) {
          el.remove();
          map.delete(q);
        }
      }
      for (let q = lo; q < hi; q++) {
        let el = map.get(q);
        if (!el) {
          el = h('div', 'sc g', `${q}<small>s${q & 7}</small>`);
          el.style.left = q * 48 + 'px';
          cells.appendChild(el);
          map.set(q, el);
        }
        el.className = 'sc ' + (q < ring.r ? 'o' : q < ring.pub ? 'u' : q < ring.w ? 'u p' : 'g');
      }
      s.style.transform = `translateX(${-(ring.r - 2) * 48 + 8}px)`;
      win.style.transform = `translateX(${ring.r * 48}px)`;
      hw.style.transform = `translateX(${ring.w * 48}px)`;
      hr.style.transform = `translateX(${ring.r * 48}px)`;
      hw.classList.toggle('blocked', ring.blocked);
    });
  })();
  (() => {
    const root = $('#inl');
    const box = h('div', 'rows');
    root.appendChild(box);
    const rows = [{
      n: 'billing/ledger',
      live: true
    }, {
      n: 'auth/session',
      c: 2,
      w: 5
    }, {
      n: 'mail/outbox',
      c: 7,
      w: 3
    }, {
      n: 'search/indexer',
      c: 0,
      w: 0
    }];
    rows.forEach(r => {
      r.el = h('div', 'row', `<span class="nm">${r.n}</span><span class="mm">${'<i></i>'.repeat(8)}</span><span class="cnt"></span>`);
      box.appendChild(r.el);
    });
    const draw = (r, w, c) => {
      [...r.el.querySelectorAll('.mm i')].forEach((el, i) => {
        const used = ((i - (w - c)) % 8 + 8) % 8 < c;
        el.className = (used ? 'u' : '') + (c < 8 && i === (w & 7) ? ' nx' : '');
      });
      const ce = r.el.querySelector('.cnt');
      ce.textContent = c + '/8';
      ce.classList.toggle('full', c >= 8);
    };
    rows.forEach(r => {
      if (!r.live) draw(r, r.w, r.c);
    });
    subs.push(e => {
      if (e.type !== 'tick') return;
      draw(rows[0], ring.w, ring.w - ring.r);
      if (ring.t % 3 === 0) rows.slice(1).forEach((r, j) => {
        const d = [[1, -1, 0, 1], [1, 1, 0, -1], [0, 1, -1, 0]][j][Math.floor(ring.t / 3) % 4];
        if (d > 0 && r.c < 8) {
          r.c++;
          r.w++;
        }
        if (d < 0 && r.c > 0) r.c--;
        draw(r, r.w, r.c);
      });
    });
  })();

  /* 1d BroadcastRing */
  (() => {
    const root = $('#bc');
    const t = new Tape(root, {
      n: 8,
      idx: [...Array(8)].map((_, i) => String(i)),
      lanes: [{
        key: 'w',
        pos: 'top'
      }, {
        key: 'r0',
        pos: 'bot',
        sm: true
      }, {
        key: 'r1',
        pos: 'bot',
        sm: true
      }, {
        key: 'r2',
        pos: 'bot',
        sm: true
      }],
      wrap: true,
      wrapLabel: 'wrap'
    });
    t.head('w', 'w', 'W', W);
    const names = ['R0 fast', 'R1', 'R2 slow'];
    names.forEach((n, i) => t.head('r' + i, 'r' + i, n, R));
    const meta = h('div', 'meta');
    root.appendChild(meta);
    const st = {
      w: 0,
      rs: [0, 0, 0],
      t: 0,
      freed: 0,
      blocked: false
    };
    let id = 1;
    const every = [1, 2, 3];
    tickers.push(() => {
      st.t++;
      const ph = st.t % 24;
      const wantW = ph < 14 && st.t % 1 === 0;
      const min = Math.min(...st.rs);
      if (wantW && st.w - st.freed >= 8) st.blocked = true;else {
        st.blocked = false;
        if (wantW) {
          const seq = st.w++,
            sl = seq & 7;
          t.set(sl, {
            val: 'e' + id++,
            on: true,
            pending: true,
            pips: [0, 0, 0]
          });
          later(D.fill, () => {
            t.set(sl, {
              pending: false,
              pub: true
            });
            later(D.step, () => t.set(sl, {
              pub: false
            }));
          });
          {
            const nx = st.w & 7;
            later(D.fill * 0.6, () => t.move('w', nx));
          }
        }
      }
      st.rs.forEach((r, i) => {
        if (st.t % every[i] === 0 && r < st.w - 1 + (st.blocked ? 1 : 0) && r < st.w) {
          const seq = r;
          st.rs[i]++;
          t.move('r' + i, st.rs[i] & 7);
          const sl = seq & 7;
          t.set(sl, {
            pips: st.rs.map(x => x > seq)
          });
        }
      });
      const nmin = Math.min(...st.rs);
      while (st.freed < nmin) {
        const sl = st.freed & 7;
        t.set(sl, {
          drain: true
        });
        later(D.drain, () => t.clear(sl));
        st.freed++;
      }
      const gate = st.rs.indexOf(nmin);
      t.heads.w.classList.toggle('blocked', st.blocked);
      t.sub('w', st.blocked ? 'blocked' : '');
      names.forEach((n, i) => {
        const g = st.blocked && i === gate;
        t.heads['r' + i].classList.toggle('blocked', g);
        t.sub('r' + i, g ? 'gating' : '');
      });
      t.slots.forEach((s, i) => t.set(i, {
        blocked: st.blocked && i === (st.w & 7)
      }));
      meta.innerHTML = st.blocked ? `<span class="warn">writer waits on R${gate}</span>` : '';
    });
  })();

  /* 1e CacheLineAccess */
  (() => {
    const root = $('#fs');
    const core = (n, role) => `<div class="core">core ${n}<small>${role}</small><em class="i">I</em></div>`;
    const scen = (cls, title, lines, label) => {
      const s = h('div', 'fs', `<div class="fs-h ${cls}">${title}</div><div class="fs-row">${core(0, 'writer · tail')}<div class="fs-mid">${lines.map((l, i) => `<div class="cl"><span>L${i}</span>${l.map(c => c ? `<i data-f="${c}">${c}</i>` : '<i class="pad"></i>').join('')}</div>`).join('')}<div class="trace ${cls === 'good' ? 'ok' : ''}"><span class="dot"></span></div></div>${core(1, 'reader · head')}</div><div class="meta"></div>`);
      root.appendChild(s);
      return s;
    };
    const bad = scen('bad', '✕ same line · struct { uint64_t tail; uint64_t head; }', [['tail', 'head', 0, 0, 0, 0, 0, 0]], 'invalidate + transfer');
    const good = scen('good', '✓ own lines · alignas(64) uint64_t tail; alignas(64) uint64_t head;', [['tail', 0, 0, 0, 0, 0, 0, 0], ['head', 0, 0, 0, 0, 0, 0, 0]], 'no traffic');
    let n = 0,
      xfer = 0,
      writes = 0;
    const ems = s => s.querySelectorAll('.core em');
    const setEm = (e, m) => {
      e.className = m ? 'm' : 'i';
      e.textContent = m ? 'M' : 'I';
    };
    tickers.push(() => {
      const c = n % 2,
        f = c === 0 ? 'tail' : 'head';
      writes++;
      [bad, good].forEach(s => {
        const cell = s.querySelector(`i[data-f="${f}"]`);
        cell.classList.add('hot');
        later(D.wrap, () => cell.classList.remove('hot'));
      });
      const be = ems(bad);
      setEm(be[c], 1);
      setEm(be[1 - c], 0);
      xfer++;
      const dot = bad.querySelector('.dot');
      dot.classList.add('nt');
      dot.style.left = c === 0 ? 'calc(100% - 8px)' : '0';
      dot.style.opacity = 1;
      reflow(dot);
      dot.classList.remove('nt');
      dot.style.left = c === 0 ? '0' : 'calc(100% - 8px)';
      later(D.wrap, () => dot.style.opacity = 0);
      const ge = ems(good);
      setEm(ge[0], 1);
      setEm(ge[1], 1);
      bad.querySelector('.meta').innerHTML = `<span>writes <b>${writes}</b></span><span style="color:var(--state-error)">line transfers <b style="color:inherit">${xfer}</b></span>`;
      good.querySelector('.meta').innerHTML = `<span>writes <b>${writes}</b></span><span>line transfers <b>0</b></span><span>cost: 128 B instead of 16 B</span>`;
      n++;
    });
  })();

  /* 2d listing, 2e reorder, 2f hex */
  const F = [{
    n: 'state',
    t: 'uint8_t',
    o: 0,
    s: 1,
    c: 1
  }, {
    p: 1,
    o: 1,
    s: 7
  }, {
    n: 'id',
    t: 'uint64_t',
    o: 8,
    s: 8,
    c: 2
  }, {
    n: 'flags',
    t: 'uint16_t',
    o: 16,
    s: 2,
    c: 3
  }, {
    p: 1,
    o: 18,
    s: 6
  }, {
    n: 'mailbox',
    t: 'void *',
    o: 24,
    s: 8,
    c: 4
  }, {
    n: 'refs',
    t: 'uint32_t',
    o: 32,
    s: 4,
    c: 6
  }, {
    n: 'prio',
    t: 'uint8_t',
    o: 36,
    s: 1,
    c: 5
  }, {
    p: 1,
    o: 37,
    s: 3
  }];
  const tint = c => `color-mix(in oklch, var(--data-${c}) 38%, var(--surface-1))`;
  (() => {
    const root = $('#ph');
    const box = h('div', 'rows');
    box.appendChild(h('div', 'ph hd', '<span>off</span><span>size</span><span>0 · 8 · 16 · 24 · 32 · 40</span><span>field</span><span>type</span>'));
    F.forEach(f => box.appendChild(h('div', 'ph' + (f.p ? ' pad' : ''), `<span>${f.o}</span><span>${f.s}</span><span class="bar"><i style="left:${f.o / 40 * 100}%;width:${f.s / 40 * 100}%"></i></span><span class="n">${f.p ? 'pad' : f.n}</span><span class="t">${f.p ? '—' : f.t}</span>`)));
    root.appendChild(box);
    root.appendChild(h('div', 'foot', 'sizeof <b>40</b> · padding <b>16 B</b> (40%) · 1 per 64 B cache line'));
  })();
  (() => {
    const root = $('#ro');
    const PX = 11;
    const box = h('div', 'ro');
    const strip = (title, fs, total) => {
      const w = h('div');
      w.appendChild(h('div', 'sub', title));
      const s = h('div', 'strip');
      s.style.width = total * PX + 'px';
      fs.forEach(f => {
        const i = h('i', f.p ? 'pad' : '', f.s >= 4 ? `${f.p ? '' : f.n}<small>${f.s} B</small>` : '');
        i.style.width = f.s * PX + 'px';
        i.title = (f.p ? 'pad' : f.n) + ' · ' + f.s + ' B';
        if (!f.p) i.style.background = tint(f.c);
        i.style.padding = f.s < 4 ? '0' : '';
        s.appendChild(i);
      });
      w.appendChild(s);
      const r = h('div', 'rul');
      r.style.width = total * PX + 'px';
      r.style.backgroundSize = '';
      r.style.background = `repeating-linear-gradient(to right,var(--fg-3) 0 1px,transparent 1px ${8 * PX}px)`;
      w.appendChild(r);
      return w;
    };
    box.appendChild(strip('Declared order · 40 B', F, 40));
    const S = [F[2], F[5], F[6], F[3], F[0], F[7]];
    box.appendChild(strip('Sorted by alignment · 24 B', S, 24));
    box.appendChild(h('div', 'delta', '−16 B per actor · 2 per 64 B cache line, was 1'));
    box.appendChild(h('div', 'foot', 'ticks every 8 B · 1-byte fields: state, prio (hover for names)'));
    root.appendChild(box);
  })();
  (() => {
    const root = $('#hx');
    const bytes = [];
    const val = {
      state: ['02'],
      id: ['2A', '00', '00', '00', '00', '00', '00', '00'],
      flags: ['11', '00'],
      mailbox: ['80', '4E', '00', '1C', '3A', '7F', '00', '00'],
      refs: ['03', '00', '00', '00'],
      prio: ['01']
    };
    F.forEach(f => {
      for (let i = 0; i < f.s; i++) bytes.push(f.p ? {
        p: 1
      } : {
        b: val[f.n][i],
        c: f.c
      });
    });
    const g = h('div', 'hx');
    for (let r = 0; r < 3; r++) {
      g.appendChild(h('span', 'o', '0x' + (r * 16).toString(16).padStart(2, '0')));
      for (let c = 0; c < 16; c++) {
        const b = bytes[r * 16 + c];
        if (!b) {
          g.appendChild(h('span'));
          continue;
        }
        const i = h('i', b.p ? 'pad' : '', b.p ? '··' : b.b);
        if (!b.p) i.style.background = tint(b.c);
        g.appendChild(i);
      }
    }
    root.appendChild(g);
    const lg = h('div', 'lg');
    [['state', 1, '2'], ['id', 2, '42 → 2A 00 … 00'], ['flags', 3, '0x0011 → 11 00'], ['mailbox', 4, '0x7f3a1c004e80 → 80 4E …'], ['refs', 6, '3'], ['prio', 5, '1']].forEach(([n, c, v]) => lg.appendChild(h('span', '', `<i style="background:${tint(c)}"></i><span><b>${n}</b> = ${v}</span>`)));
    root.appendChild(lg);
  })();

  /* Section 3 primitives */
  (() => {
    const root = $('#prims');
    const card = (id, title, tok) => {
      const f = h('section', 'frame');
      f.id = id;
      f.dataset.screenLabel = id + ' ' + title;
      f.innerHTML = `<div class="fh"><span class="id">${id}</span><h3>${title}</h3></div><div class="tok">${tok}</div>`;
      root.appendChild(f);
      return f;
    };
    const idx6 = ['0', '1', '2', '3', '4', '5'];
    {
      const f = card('3a', 'step', '<span><b>step</b> 420ms</span><span><b>in-out</b></span>');
      const t = new Tape(f, {
        n: 6,
        sw: 40,
        idx: idx6,
        lanes: [{
          key: 't',
          pos: 'top'
        }]
      });
      t.head('h', 't', 'R', R);
      ['a', 'b', 'c', 'd', 'e', 'f'].forEach((v, i) => t.set(i, {
        val: v,
        on: true
      }));
      let i = 0;
      tickers.push(() => {
        i = (i + 1) % 7;
        if (i === 6) return;
        t.move('h', i, i === 0);
      });
    }
    {
      const f = card('3b', 'fill → publish', '<span><b>fill</b> 480ms decelerate</span><span><b>publish</b> 240ms in · 900ms out</span>');
      const t = new Tape(f, {
        n: 6,
        sw: 40,
        idx: idx6,
        lanes: [{
          key: 't',
          pos: 'top'
        }]
      });
      t.head('h', 't', 'W', W);
      let i = 0;
      tickers.push(() => {
        if (i === 6) {
          for (let j = 0; j < 6; j++) t.fadeClear(j);
          t.move('h', 0, true);
          i = 0;
          return;
        }
        const s = i;
        t.set(s, {
          val: 'm' + s,
          on: true,
          pending: true
        });
        later(D.fill, () => {
          t.set(s, {
            pending: false,
            pub: true
          });
          later(D.step, () => t.set(s, {
            pub: false
          }));
        });
        i++;
        const ni = i;
        later(D.fill * 0.6, () => {
          if (ni < 6) t.move('h', ni);
        });
      });
    }
    {
      const f = card('3c', 'drain', '<span><b>drain</b> 360ms accelerate</span><span>8px toward head</span>');
      const t = new Tape(f, {
        n: 6,
        sw: 40,
        idx: idx6,
        lanes: [{
          key: 't',
          pos: 'bot'
        }]
      });
      t.head('h', 't', 'R', R);
      const fill = () => {
        for (let j = 0; j < 6; j++) {
          t.clear(j);
          t.set(j, {
            val: 'm' + j,
            on: true
          });
        }
      };
      fill();
      let i = 0;
      tickers.push(() => {
        if (i === 6) {
          fill();
          t.move('h', 0, true);
          i = 0;
          return;
        }
        const s = i;
        t.set(s, {
          drain: true
        });
        later(D.drain, () => t.clear(s));
        i++;
        const ni = i;
        if (ni < 6) later(D.drain * 0.5, () => t.move('h', ni));
      });
    }
    {
      const f = card('3d', 'block', '<span><b>fade</b> 240ms, then hold</span><span><b>--state-waiting</b></span>');
      const t = new Tape(f, {
        n: 6,
        sw: 40,
        idx: idx6,
        lanes: [{
          key: 'w',
          pos: 'top'
        }, {
          key: 'r',
          pos: 'bot'
        }]
      });
      t.head('w', 'w', 'W', W);
      t.head('r', 'r', 'R', R);
      for (let j = 0; j < 6; j++) t.set(j, {
        val: 'm' + j,
        on: true
      });
      let p = 0,
        c = 0,
        id = 6;
      const blk = () => {
        t.heads.w.classList.add('blocked');
        t.sub('w', 'blocked');
        t.set(p, {
          blocked: true
        });
      };
      blk();
      tickers.push(() => {
        c++;
        if (c % 4 === 3) {
          const s = p;
          t.set(s, {
            drain: true,
            blocked: false
          });
          later(D.drain, () => t.clear(s));
          t.move('r', (p + 1) % 6);
          t.heads.w.classList.remove('blocked');
          t.sub('w', '');
        } else if (c % 4 === 0) {
          t.set(p, {
            val: 'm' + id++,
            on: true
          });
          p = (p + 1) % 6;
          t.move('w', p, p === 0);
          blk();
        }
      });
      t.move('w', 0);
      t.move('r', 0);
    }
    {
      const f = card('3e', 'wrap', '<span><b>exit · enter</b> 240ms</span><span><b>pulse</b> 700ms</span>');
      const t = new Tape(f, {
        n: 6,
        sw: 40,
        idx: idx6,
        lanes: [{
          key: 't',
          pos: 'top'
        }],
        wrap: true,
        wrapLabel: 'seq % 6'
      });
      t.head('h', 't', 'W', W);
      let i = 0;
      tickers.push(() => {
        i = (i + 1) % 6;
        t.move('h', i);
      });
    }
    {
      const f = card('3f', 'transfer', '<span><b>transfer</b> 700ms in-out</span><span><b>--state-error</b></span>');
      const m = h('div', 'mini', '<div class="core">core 0<em class="m">M</em></div><div class="trace"><span class="dot"></span><span class="x">line moves</span></div><div class="core">core 1<em class="i">I</em></div>');
      f.appendChild(m);
      f.appendChild(h('div', 'foot', 'Only for coherence traffic. Never for ordinary data flow.'));
      const ems = m.querySelectorAll('em'),
        dot = m.querySelector('.dot');
      let c = 0;
      tickers.push(() => {
        if (c % 2 === 0) {
          const to = c / 2 % 2 === 0 ? 1 : 0;
          ems[to].className = 'm';
          ems[to].textContent = 'M';
          ems[1 - to].className = 'i';
          ems[1 - to].textContent = 'I';
          dot.classList.add('nt');
          dot.style.left = to === 1 ? '0' : 'calc(100% - 8px)';
          dot.style.opacity = 1;
          reflow(dot);
          dot.classList.remove('nt');
          dot.style.left = to === 1 ? 'calc(100% - 8px)' : '0';
          later(D.wrap, () => dot.style.opacity = 0);
        }
        c++;
      });
    }
  })();
  $('#bPause').onclick = e => {
    paused = !paused;
    e.target.textContent = paused ? 'Play' : 'Pause';
    start();
  };
  $('#bSpeed').onclick = e => {
    K = K === 1 ? 4 : 1;
    document.documentElement.style.setProperty('--k', K);
    e.target.textContent = 'Speed ' + (K === 1 ? '1×' : '¼×');
    start();
  };
  $('#bTheme').onclick = e => {
    const d = document.documentElement.dataset.theme === 'dark';
    if (d) delete document.documentElement.dataset.theme;else document.documentElement.dataset.theme = 'dark';
    e.target.textContent = d ? 'Dark' : 'Light';
  };
  tickers.forEach(f => f());
  start();
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "explorations/memory-sketches.js", error: String((e && e.message) || e) }); }

// scripts/build-bundle.cjs
try { (() => {
// Rebuilds _ds_bundle.js outside Claude Design so component cards and UI kits render from a clone.
// Usage: npm install && npm run build
const {
  build
} = require('esbuild');
const {
  readdirSync,
  statSync
} = require('fs');
const {
  join,
  relative
} = require('path');
const NS = 'TinyactorsDesignSystem_2f489e';
const root = join(__dirname, '..');
const walk = dir => readdirSync(dir).flatMap(f => {
  const p = join(dir, f);
  return statSync(p).isDirectory() ? walk(p) : /^[A-Z]\w*\.jsx$/.test(f) ? [p] : [];
});
const files = walk(join(root, 'components'));
const entry = files.map(f => `export * from './${relative(root, f)}';`).join('\n');
const reactGlobal = {
  name: 'react-global',
  setup(b) {
    b.onResolve({
      filter: /^react$/
    }, () => ({
      path: 'react',
      namespace: 'react-global'
    }));
    b.onLoad({
      filter: /.*/,
      namespace: 'react-global'
    }, () => ({
      contents: 'module.exports = window.React;',
      loader: 'js'
    }));
  }
};
build({
  stdin: {
    contents: entry,
    resolveDir: root,
    loader: 'js'
  },
  bundle: true,
  format: 'iife',
  globalName: NS,
  outfile: join(root, '_ds_bundle.js'),
  jsx: 'transform',
  jsxFactory: 'React.createElement',
  jsxFragment: 'React.Fragment',
  banner: {
    js: '/* generated by scripts/build-bundle.mjs: do not edit */'
  },
  plugins: [reactGlobal],
  logLevel: 'info'
}).then(() => console.log(`bundled ${files.length} component files into window.${NS}`));
})(); } catch (e) { __ds_ns.__errors.push({ path: "scripts/build-bundle.cjs", error: String((e && e.message) || e) }); }

// ui_kits/inspector/ActorTree.jsx
try { (() => {
(() => {
  const {
    Badge,
    Input,
    Icon
  } = window.TinyactorsDesignSystem_2f489e;
  function ActorTree({
    selected,
    onSelect,
    tones
  }) {
    return /*#__PURE__*/React.createElement("aside", {
      style: {
        width: 260,
        flex: 'none',
        borderRight: '1px solid var(--border-1)',
        background: 'var(--surface-1)',
        display: 'flex',
        flexDirection: 'column',
        minHeight: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        padding: 12,
        borderBottom: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement(Input, {
      icon: "search",
      placeholder: "Filter actors",
      size: "sm",
      mono: true
    })), /*#__PURE__*/React.createElement("div", {
      style: {
        padding: '10px 12px 6px',
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-4)'
      }
    }, "actors \xB7 ", window.INSPECTOR_TREE.length), /*#__PURE__*/React.createElement("div", {
      style: {
        overflow: 'auto',
        padding: '0 6px 12px'
      }
    }, window.INSPECTOR_TREE.map(a => {
      const sel = a.id === selected,
        tone = tones[a.id] || a.tone;
      return /*#__PURE__*/React.createElement("div", {
        key: a.id,
        onClick: () => onSelect(a.id),
        style: {
          display: 'flex',
          alignItems: 'center',
          gap: 8,
          height: 30,
          paddingLeft: 8 + a.depth * 16,
          paddingRight: 8,
          borderRadius: 'var(--radius-2)',
          cursor: 'pointer',
          background: sel ? 'var(--surface-3)' : 'transparent',
          color: sel ? 'var(--fg-1)' : 'var(--fg-2)'
        }
      }, /*#__PURE__*/React.createElement(Icon, {
        name: a.kind === 'supervisor' ? 'network' : 'circle-dot',
        size: 14,
        style: {
          color: 'var(--fg-4)'
        }
      }), /*#__PURE__*/React.createElement("span", {
        style: {
          font: '400 12px/1 var(--font-mono)',
          flex: 1,
          minWidth: 0,
          overflow: 'hidden',
          textOverflow: 'ellipsis'
        }
      }, a.name), a.kind !== 'supervisor' && /*#__PURE__*/React.createElement(Badge, {
        tone: tone
      }));
    })));
  }
  window.ActorTree = ActorTree;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/inspector/ActorTree.jsx", error: String((e && e.message) || e) }); }

// ui_kits/inspector/ChartView.jsx
try { (() => {
(() => {
  const {
    Tag,
    Input,
    Button,
    Tabs,
    Sheet
  } = window.TinyactorsDesignSystem_2f489e;
  function ChartView({
    actor,
    state,
    onSend
  }) {
    const C = window.INSPECTOR_CHART;
    const [ev, setEv] = React.useState('');
    const avail = Object.keys(C.on[state] || {});
    return /*#__PURE__*/React.createElement("section", {
      style: {
        flex: 1,
        minWidth: 0,
        overflow: 'hidden',
        display: 'flex',
        flexDirection: 'column'
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        alignItems: 'center',
        gap: 12,
        padding: '0 16px',
        height: 44,
        flex: 'none',
        whiteSpace: 'nowrap',
        overflow: 'hidden',
        borderBottom: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 13px var(--font-mono)',
        flex: 'none'
      }
    }, actor), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-4)',
        minWidth: 0,
        overflow: 'hidden',
        textOverflow: 'ellipsis'
      }
    }, "chart: ", C.id, ".scxml"), /*#__PURE__*/React.createElement("div", {
      style: {
        marginLeft: 'auto',
        flex: 'none'
      }
    }, /*#__PURE__*/React.createElement(Tabs, {
      variant: "segmented",
      items: ['Chart', 'Tree', 'JSON']
    }))), /*#__PURE__*/React.createElement(Sheet, {
      grid: true,
      style: {
        flex: 1,
        minHeight: 0
      },
      bodyStyle: {
        display: 'flex',
        flexDirection: 'column',
        alignItems: 'center',
        justifyContent: 'center',
        gap: 10,
        padding: 24
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-4)'
      }
    }, "current state"), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 28px/1 var(--font-mono)',
        color: 'var(--accent-fg)'
      }
    }, state), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-4)'
      }
    }, "statechart view: component pending")), /*#__PURE__*/React.createElement("div", {
      style: {
        borderTop: '1px solid var(--border-1)',
        padding: 12,
        display: 'flex',
        flexWrap: 'wrap',
        gap: 10,
        alignItems: 'center',
        flex: 'none',
        background: 'var(--surface-1)'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-4)'
      }
    }, "send"), avail.map(e => /*#__PURE__*/React.createElement(Tag, {
      key: e,
      glyph: "\u2192",
      style: {
        cursor: 'pointer'
      },
      onClick: () => onSend(e)
    }, e)), !avail.length && /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-4)'
      }
    }, "final \u2014 no transitions"), /*#__PURE__*/React.createElement("form", {
      style: {
        marginLeft: 'auto',
        display: 'flex',
        gap: 8,
        minWidth: 0
      },
      onSubmit: e => {
        e.preventDefault();
        if (ev) {
          onSend(ev.toUpperCase());
          setEv('');
        }
      }
    }, /*#__PURE__*/React.createElement(Input, {
      mono: true,
      size: "sm",
      placeholder: "EVENT.NAME",
      value: ev,
      onChange: e => setEv(e.target.value),
      style: {
        width: 180,
        maxWidth: '100%',
        minWidth: 0
      }
    }), /*#__PURE__*/React.createElement(Button, {
      type: "submit",
      size: "sm",
      variant: "primary",
      icon: "send"
    }, "Send"))));
  }
  window.ChartView = ChartView;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/inspector/ChartView.jsx", error: String((e && e.message) || e) }); }

// ui_kits/inspector/EventLog.jsx
try { (() => {
(() => {
  const {
    Tabs,
    Badge,
    Switch
  } = window.TinyactorsDesignSystem_2f489e;
  function EventLog({
    log,
    state,
    actor
  }) {
    const [tab, setTab] = React.useState('events');
    return /*#__PURE__*/React.createElement("aside", {
      style: {
        width: 340,
        flex: 'none',
        borderLeft: '1px solid var(--border-1)',
        background: 'var(--surface-1)',
        display: 'flex',
        flexDirection: 'column',
        minHeight: 0
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        padding: '0 8px'
      }
    }, /*#__PURE__*/React.createElement(Tabs, {
      value: tab,
      onChange: setTab,
      items: [{
        value: 'events',
        label: 'Events',
        count: log.length
      }, {
        value: 'state',
        label: 'State'
      }, {
        value: 'mailbox',
        label: 'Mailbox',
        count: 0
      }]
    })), tab === 'events' && /*#__PURE__*/React.createElement(React.Fragment, null, /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        justifyContent: 'space-between',
        alignItems: 'center',
        padding: '10px 12px'
      }
    }, /*#__PURE__*/React.createElement(Switch, {
      label: "Auto-scroll",
      defaultChecked: true
    }), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 11px var(--font-mono)',
        color: 'var(--fg-4)'
      }
    }, "t = virtual")), /*#__PURE__*/React.createElement("div", {
      style: {
        overflow: 'auto',
        flex: 1,
        font: '400 12px/1 var(--font-mono)'
      }
    }, log.slice().reverse().map((r, i) => /*#__PURE__*/React.createElement("div", {
      key: log.length - i,
      style: {
        display: 'grid',
        gridTemplateColumns: '52px 1fr',
        gap: 8,
        padding: '9px 12px',
        borderTop: '1px solid var(--border-1)',
        background: i === 0 ? 'color-mix(in srgb, var(--accent) 8%, transparent)' : 'transparent'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        color: 'var(--fg-4)'
      }
    }, r.t), /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        flexDirection: 'column',
        gap: 6
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        color: r.ok ? 'var(--fg-1)' : 'var(--state-error)'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        color: 'var(--accent-fg)'
      }
    }, "\u2192 "), r.ev), /*#__PURE__*/React.createElement("span", {
      style: {
        color: 'var(--fg-3)'
      }
    }, r.ok ? r.from + ' ⇢ ' + r.to : 'ignored in ' + r.from)))))), tab !== 'events' && /*#__PURE__*/React.createElement("pre", {
      style: {
        margin: 0,
        padding: 16,
        font: 'var(--type-code)',
        color: 'var(--fg-2)'
      }
    }, tab === 'state' ? JSON.stringify({
      actor,
      configuration: ['checkout', state],
      history: {},
      data: {
        items: 2,
        attempts: log.filter(l => l.ev === 'RETRY').length
      }
    }, null, 2) : '// mailbox empty'));
  }
  window.EventLog = EventLog;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/inspector/EventLog.jsx", error: String((e && e.message) || e) }); }

// ui_kits/inspector/Toolbar.jsx
try { (() => {
(() => {
  const {
    IconButton,
    Button,
    Badge,
    Kbd,
    Tooltip
  } = window.TinyactorsDesignSystem_2f489e;
  function Toolbar({
    paused,
    setPaused,
    onReset,
    step
  }) {
    return /*#__PURE__*/React.createElement("header", {
      style: {
        height: 48,
        flex: 'none',
        display: 'flex',
        alignItems: 'center',
        gap: 16,
        padding: '0 12px 0 16px',
        borderBottom: '1px solid var(--border-1)',
        background: 'var(--bg-app)'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 15px/1 var(--font-mono)',
        letterSpacing: '-0.04em',
        flex: 'none'
      }
    }, "tinyactors"), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-3)',
        whiteSpace: 'nowrap',
        overflow: 'hidden',
        textOverflow: 'ellipsis',
        minWidth: 0
      }
    }, "inspector \xB7 ", /*#__PURE__*/React.createElement("span", {
      style: {
        color: 'var(--fg-1)'
      }
    }, "shop-dev"), " \xB7 pid 48213"), /*#__PURE__*/React.createElement(Badge, {
      tone: paused ? 'waiting' : 'running'
    }, paused ? 'paused' : 'live'), /*#__PURE__*/React.createElement("div", {
      style: {
        marginLeft: 'auto',
        display: 'flex',
        alignItems: 'center',
        gap: 4
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-4)',
        marginRight: 8,
        whiteSpace: 'nowrap'
      }
    }, "step ", step), /*#__PURE__*/React.createElement(Tooltip, {
      side: "bottom",
      content: "Pause scheduler"
    }, /*#__PURE__*/React.createElement(IconButton, {
      icon: paused ? 'play' : 'pause',
      label: paused ? 'Resume' : 'Pause',
      onClick: () => setPaused(!paused)
    })), /*#__PURE__*/React.createElement(Tooltip, {
      side: "bottom",
      content: "Step one event"
    }, /*#__PURE__*/React.createElement(IconButton, {
      icon: "skip-forward",
      label: "Step",
      disabled: !paused
    })), /*#__PURE__*/React.createElement(Tooltip, {
      side: "bottom",
      content: "Reset to initial"
    }, /*#__PURE__*/React.createElement(IconButton, {
      icon: "rotate-ccw",
      label: "Reset",
      onClick: onReset
    })), /*#__PURE__*/React.createElement("span", {
      style: {
        width: 1,
        height: 20,
        background: 'var(--border-1)',
        margin: '0 8px'
      }
    }), /*#__PURE__*/React.createElement(Button, {
      size: "sm",
      icon: "download"
    }, "Export trace"), /*#__PURE__*/React.createElement("span", {
      style: {
        display: 'flex',
        gap: 3,
        marginLeft: 8
      }
    }, /*#__PURE__*/React.createElement(Kbd, null, "\u2318"), /*#__PURE__*/React.createElement(Kbd, null, "K"))));
  }
  window.Toolbar = Toolbar;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/inspector/Toolbar.jsx", error: String((e && e.message) || e) }); }

// ui_kits/inspector/data.js
try { (() => {
window.INSPECTOR_CHART = {
  id: 'checkout',
  states: ['cart', 'paying', 'failed', 'paid'],
  final: ['paid'],
  on: {
    cart: {
      ADD_ITEM: 'cart',
      CHECKOUT: 'paying'
    },
    paying: {
      'PAYMENT.OK': 'paid',
      'PAYMENT.FAIL': 'failed'
    },
    failed: {
      RETRY: 'paying',
      CANCEL: 'cart'
    },
    paid: {}
  }
};
window.INSPECTOR_TREE = [{
  id: 'root',
  name: 'app.sup',
  kind: 'supervisor',
  tone: 'running',
  depth: 0
}, {
  id: 'sess',
  name: 'session.sup',
  kind: 'supervisor',
  tone: 'running',
  depth: 1
}, {
  id: 'checkout.1',
  name: 'checkout.1',
  kind: 'checkout',
  tone: 'running',
  depth: 2
}, {
  id: 'checkout.2',
  name: 'checkout.2',
  kind: 'checkout',
  tone: 'waiting',
  depth: 2
}, {
  id: 'checkout.3',
  name: 'checkout.3',
  kind: 'checkout',
  tone: 'error',
  depth: 2
}, {
  id: 'mailer',
  name: 'mailer',
  kind: 'mailer',
  tone: 'waiting',
  depth: 1
}, {
  id: 'ledger',
  name: 'ledger',
  kind: 'ledger',
  tone: 'done',
  depth: 1
}, {
  id: 'metrics',
  name: 'metrics',
  kind: 'metrics',
  tone: 'stopped',
  depth: 1
}];
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/inspector/data.js", error: String((e && e.message) || e) }); }

// ui_kits/website/DocsPage.jsx
try { (() => {
(() => {
  const {
    CodeBlock,
    Input,
    Tabs,
    Badge,
    Button
  } = window.TinyactorsDesignSystem_2f489e;
  const DOC_NAV = [['Getting started', ['Install', 'First actor', 'Sending events']], ['Concepts', ['Statecharts', 'Mailboxes', 'Supervision', 'Timers']], ['Semantics', ['Event processing', 'Entry & exit sets', 'Conformance']], ['Bindings', ['C', 'JavaScript']]];
  const SNIPS = {
    C: '#include <tinyactors.h>\n\nta_system *sys = ta_system_new(NULL);\nta_actor *door = ta_spawn(sys, &door_chart, NULL);\nta_send(door, "OPEN", NULL);\nassert(ta_in(door, "opened"));',
    JavaScript: 'import { System } from "tinyactors";\n\nconst sys = new System();\nconst door = sys.spawn(doorChart);\ndoor.send("OPEN");\nassert(door.in("opened"));'
  };
  function DocsPage() {
    const [cur, setCur] = React.useState('First actor');
    const [lang, setLang] = React.useState('C');
    return /*#__PURE__*/React.createElement("div", {
      style: {
        maxWidth: 'var(--content-max)',
        margin: '0 auto',
        padding: '0 24px',
        display: 'grid',
        gridTemplateColumns: 'var(--sidebar-w) minmax(0,1fr) 180px',
        gap: 48
      }
    }, /*#__PURE__*/React.createElement("aside", {
      style: {
        padding: '32px 0',
        borderRight: '1px solid var(--border-1)',
        paddingRight: 24,
        display: 'flex',
        flexDirection: 'column',
        gap: 24
      }
    }, /*#__PURE__*/React.createElement(Input, {
      icon: "search",
      placeholder: "Search docs",
      size: "sm"
    }), DOC_NAV.map(([g, items]) => /*#__PURE__*/React.createElement("div", {
      key: g,
      style: {
        display: 'flex',
        flexDirection: 'column',
        gap: 2
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-4)',
        marginBottom: 8
      }
    }, g), items.map(i => /*#__PURE__*/React.createElement("a", {
      key: i,
      href: "#",
      onClick: e => {
        e.preventDefault();
        setCur(i);
      },
      style: {
        font: '400 14px/1 var(--font-sans)',
        padding: '7px 10px',
        borderRadius: 'var(--radius-2)',
        textDecoration: 'none',
        color: cur === i ? 'var(--fg-1)' : 'var(--fg-2)',
        background: cur === i ? 'var(--surface-2)' : 'transparent'
      }
    }, i))))), /*#__PURE__*/React.createElement("article", {
      style: {
        padding: '40px 0 80px',
        maxWidth: 'var(--prose-max)'
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-3)'
      }
    }, "Getting started / ", cur), /*#__PURE__*/React.createElement("h1", {
      style: {
        font: 'var(--type-h1)',
        letterSpacing: 'var(--tracking-display)',
        marginTop: 16
      }
    }, cur), /*#__PURE__*/React.createElement("p", {
      style: {
        font: 'var(--type-prose)',
        color: 'var(--fg-1)',
        marginTop: 20
      }
    }, "An actor is a statechart with a mailbox. You describe the chart once, spawn as many instances as you need, and talk to them only by sending events. Each actor processes one event at a time, to completion."), /*#__PURE__*/React.createElement("h2", {
      style: {
        font: 'var(--type-h2)',
        marginTop: 40
      }
    }, "Spawn and send"), /*#__PURE__*/React.createElement("p", {
      style: {
        font: 'var(--type-prose)',
        color: 'var(--fg-1)',
        marginTop: 12
      }
    }, "The same four calls exist in C and JavaScript. ", /*#__PURE__*/React.createElement("code", {
      style: {
        font: '400 15px var(--font-mono)',
        color: 'var(--fg-1)',
        background: 'var(--surface-2)',
        padding: '2px 5px',
        borderRadius: 2
      }
    }, "send"), " enqueues; the scheduler delivers on the next step."), /*#__PURE__*/React.createElement("div", {
      style: {
        marginTop: 20
      }
    }, /*#__PURE__*/React.createElement(Tabs, {
      variant: "segmented",
      items: Object.keys(SNIPS),
      value: lang,
      onChange: setLang
    })), /*#__PURE__*/React.createElement(CodeBlock, {
      style: {
        marginTop: 10
      },
      filename: 'door.' + {
        C: 'c',
        JavaScript: 'js'
      }[lang],
      code: SNIPS[lang],
      highlight: [5]
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        marginTop: 28,
        padding: '14px 16px',
        border: '1px solid var(--border-1)',
        borderRadius: 'var(--radius-2)',
        background: 'var(--surface-1)',
        display: 'flex',
        gap: 12,
        alignItems: 'baseline'
      }
    }, /*#__PURE__*/React.createElement(Badge, {
      tone: "waiting",
      dot: false
    }, "note"), /*#__PURE__*/React.createElement("p", {
      style: {
        font: 'var(--type-body-sm)',
        color: 'var(--fg-2)'
      }
    }, "Events sent from inside a transition are queued as internal events and processed before the next external one, as SCXML specifies.")), /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        justifyContent: 'space-between',
        marginTop: 48,
        paddingTop: 20,
        borderTop: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement(Button, {
      variant: "ghost",
      icon: "arrow-left"
    }, "Install"), /*#__PURE__*/React.createElement(Button, {
      variant: "ghost",
      iconRight: "arrow-right"
    }, "Sending events"))), /*#__PURE__*/React.createElement("nav", {
      style: {
        padding: '40px 0',
        display: 'flex',
        flexDirection: 'column',
        gap: 10,
        font: '400 13px var(--font-sans)',
        color: 'var(--fg-3)'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-4)'
      }
    }, "on this page"), /*#__PURE__*/React.createElement("span", {
      style: {
        color: 'var(--fg-1)'
      }
    }, "Spawn and send"), /*#__PURE__*/React.createElement("span", null, "Asserting state"), /*#__PURE__*/React.createElement("span", null, "Next steps")));
  }
  window.DocsPage = DocsPage;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/website/DocsPage.jsx", error: String((e && e.message) || e) }); }

// ui_kits/website/Features.jsx
try { (() => {
(() => {
  const {
    Tag,
    Icon
  } = window.TinyactorsDesignSystem_2f489e;
  const HALLMARKS = [['boxes', 'Millions of actors', 'Run millions of actors in a single system. An idle actor costs its state and an empty mailbox, nothing more.'], ['memory-stick', 'Transparent paging', 'Actors page in and out of RAM transparently. Cold actors move to disk and wake on their next event with the same state.'], ['gauge', 'Hundreds of thousands of events/s', 'The scheduler handles hundreds of thousands of events per second, each processed to completion.'], ['activity', 'OpenTelemetry built in', 'Every send, transition and invoke emits OpenTelemetry spans out of the box. Point it at your collector.']];
  const FEATURES = [['file-check-2', 'Defined semantics', 'Event processing follows the SCXML algorithm: run-to-completion, document-order transitions, explicit entry and exit sets. No undefined behavior between C and JavaScript.'], ['languages', 'C and JavaScript', 'One C ABI, no runtime of its own. The JavaScript package is a thin wrapper, so an actor spawned from Node or Bun behaves exactly like one spawned from C.'], ['timer', 'Fast verify loops', 'Deterministic scheduler and virtual clock. Replay a trace, step one event, assert the configuration. Tests finish in milliseconds.'], ['bot', 'Legible to agents', 'Charts are data. State, mailbox and history serialize to JSON, so a coding agent can read a failure and propose the fix.']];
  function Features() {
    return /*#__PURE__*/React.createElement("section", {
      style: {
        maxWidth: 'var(--content-max)',
        margin: '0 auto',
        padding: '0 24px'
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'grid',
        gridTemplateColumns: 'repeat(auto-fit, minmax(220px,1fr))',
        borderLeft: '1px solid var(--border-1)'
      }
    }, [...HALLMARKS, ...FEATURES].map(([ic, t, d]) => /*#__PURE__*/React.createElement("div", {
      key: t,
      style: {
        padding: '40px 24px',
        borderRight: '1px solid var(--border-1)',
        borderBottom: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement(Icon, {
      name: ic,
      size: 20,
      style: {
        color: 'var(--accent-fg)'
      }
    }), /*#__PURE__*/React.createElement("h3", {
      style: {
        font: 'var(--type-h3)',
        marginTop: 20
      }
    }, t), /*#__PURE__*/React.createElement("p", {
      style: {
        font: 'var(--type-body-sm)',
        color: 'var(--fg-2)',
        marginTop: 8
      }
    }, d)))));
  }
  function Bindings() {
    const langs = ['C', 'JavaScript · Node', 'JavaScript · Bun'];
    return /*#__PURE__*/React.createElement("section", {
      style: {
        borderTop: '1px solid var(--border-1)',
        borderBottom: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        maxWidth: 'var(--content-max)',
        margin: '0 auto',
        padding: '28px 24px',
        display: 'flex',
        alignItems: 'center',
        gap: 24
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-3)',
        flex: 'none'
      }
    }, "supported"), /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        flexWrap: 'wrap',
        gap: 8
      }
    }, langs.map(l => /*#__PURE__*/React.createElement(Tag, {
      key: l
    }, l)))));
  }
  function SiteFooter() {
    return /*#__PURE__*/React.createElement("footer", {
      style: {
        maxWidth: 'var(--content-max)',
        margin: '0 auto',
        padding: '40px 24px 56px',
        display: 'flex',
        gap: 24,
        alignItems: 'center',
        font: '400 12px var(--font-mono)',
        color: 'var(--fg-3)'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 14px var(--font-mono)',
        letterSpacing: '-0.04em',
        color: 'var(--fg-2)'
      }
    }, "tinyactors"), /*#__PURE__*/React.createElement("span", null, "MIT"), /*#__PURE__*/React.createElement("span", null, "Docs"), /*#__PURE__*/React.createElement("span", null, "Semantics"), /*#__PURE__*/React.createElement("span", null, "GitHub"), /*#__PURE__*/React.createElement("span", {
      style: {
        marginLeft: 'auto'
      }
    }, "v0.4.2"));
  }
  Object.assign(window, {
    Features,
    Bindings,
    SiteFooter
  });
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/website/Features.jsx", error: String((e && e.message) || e) }); }

// ui_kits/website/Header.jsx
try { (() => {
(() => {
  const {
    Button,
    Badge,
    Icon
  } = window.TinyactorsDesignSystem_2f489e;
  function SiteHeader({
    page,
    go
  }) {
    const link = (id, label) => /*#__PURE__*/React.createElement("a", {
      href: "#",
      onClick: e => {
        e.preventDefault();
        go(id);
      },
      style: {
        font: 'var(--type-ui)',
        color: page === id ? 'var(--fg-1)' : 'var(--fg-3)',
        textDecoration: 'none'
      }
    }, label);
    return /*#__PURE__*/React.createElement("header", {
      style: {
        position: 'sticky',
        top: 0,
        zIndex: 10,
        background: 'color-mix(in srgb, var(--bg-app) 88%, transparent)',
        backdropFilter: 'blur(8px)',
        borderBottom: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        maxWidth: 'var(--content-max)',
        margin: '0 auto',
        padding: '0 24px',
        height: 56,
        display: 'flex',
        alignItems: 'center',
        gap: 28
      }
    }, /*#__PURE__*/React.createElement("a", {
      href: "#",
      onClick: e => {
        e.preventDefault();
        go('home');
      },
      style: {
        font: '500 18px/1 var(--font-mono)',
        letterSpacing: '-0.04em',
        color: 'var(--fg-1)',
        textDecoration: 'none'
      }
    }, "tinyactors"), /*#__PURE__*/React.createElement(Badge, {
      dot: false
    }, "v0.4.2"), /*#__PURE__*/React.createElement("nav", {
      style: {
        display: 'flex',
        gap: 22,
        marginLeft: 8
      }
    }, link('docs', 'Docs'), link('semantics', 'Semantics'), link('bindings', 'Bindings'), link('blog', 'Changelog')), /*#__PURE__*/React.createElement("div", {
      style: {
        marginLeft: 'auto',
        display: 'flex',
        gap: 8,
        alignItems: 'center'
      }
    }, /*#__PURE__*/React.createElement(Button, {
      variant: "ghost",
      size: "sm",
      icon: "github"
    }, "GitHub"), /*#__PURE__*/React.createElement(Button, {
      variant: "primary",
      size: "sm",
      onClick: () => go('docs')
    }, "Install"))));
  }
  window.SiteHeader = SiteHeader;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/website/Header.jsx", error: String((e && e.message) || e) }); }

// ui_kits/website/Hero.jsx
try { (() => {
(() => {
  const {
    Button,
    CodeBlock,
    Tag,
    Sheet
  } = window.TinyactorsDesignSystem_2f489e;
  const HERO_CODE = 'import { System } from "tinyactors";\n\nconst sys = new System();\nconst door = sys.spawn(doorChart);\n\ndoor.send("OPEN");\nassert(door.in("opened"));';
  function Hero({
    go
  }) {
    const events = {
      closed: ['OPEN', 'LOCK'],
      opened: ['CLOSE'],
      locked: ['UNLOCK']
    };
    const next = {
      OPEN: 'opened',
      CLOSE: 'closed',
      LOCK: 'locked',
      UNLOCK: 'closed'
    };
    const [s, setS] = React.useState('closed');
    return /*#__PURE__*/React.createElement("section", {
      style: {
        borderBottom: '1px solid var(--border-1)'
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        maxWidth: 'var(--content-max)',
        margin: '0 auto',
        padding: '96px 24px 88px',
        display: 'grid',
        gridTemplateColumns: 'minmax(0,1.05fr) minmax(0,1fr)',
        gap: 56,
        alignItems: 'center'
      }
    }, /*#__PURE__*/React.createElement("div", null, /*#__PURE__*/React.createElement("div", {
      style: {
        font: 'var(--type-label)',
        letterSpacing: 'var(--tracking-label)',
        textTransform: 'uppercase',
        color: 'var(--fg-3)',
        display: 'flex',
        alignItems: 'center',
        gap: 10
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        width: 28,
        height: 1,
        background: 'var(--fg-3)'
      }
    }), "embeddable actor runtime"), /*#__PURE__*/React.createElement("h1", {
      style: {
        font: '400 80px/0.98 var(--font-display)',
        letterSpacing: 'var(--tracking-display)',
        margin: '20px 0 0'
      }
    }, "Actors,", /*#__PURE__*/React.createElement("br", null), "as a ", /*#__PURE__*/React.createElement("em", null, "library"), "."), /*#__PURE__*/React.createElement("p", {
      style: {
        font: '400 18px/1.55 var(--font-sans)',
        color: 'var(--fg-2)',
        marginTop: 24,
        maxWidth: 500
      }
    }, "A small C-ABI runtime for supervised state machines. Supervision from Elixir, statecharts from XState, semantics from SCXML. Use it from C or JavaScript (Node and Bun)."), /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        gap: 10,
        marginTop: 32
      }
    }, /*#__PURE__*/React.createElement(Button, {
      variant: "primary",
      size: "lg",
      iconRight: "arrow-right",
      onClick: () => go('docs')
    }, "Get started"), /*#__PURE__*/React.createElement(Button, {
      size: "lg",
      icon: "book-open",
      onClick: () => go('docs')
    }, "Read the semantics")), /*#__PURE__*/React.createElement("div", {
      style: {
        marginTop: 28,
        font: '400 13px var(--font-mono)',
        color: 'var(--fg-3)'
      }
    }, "$ npm install tinyactors")), /*#__PURE__*/React.createElement(Sheet, {
      grid: true,
      parallax: true,
      bodyStyle: {
        padding: 20,
        display: 'flex',
        flexDirection: 'column',
        gap: 16
      }
    }, /*#__PURE__*/React.createElement(CodeBlock, {
      filename: "door.test.js",
      highlight: [6, 7],
      code: HERO_CODE
    }), /*#__PURE__*/React.createElement("div", {
      style: {
        padding: '8px 4px 0',
        display: 'flex',
        flexDirection: 'column',
        gap: 16
      }
    }, /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        alignItems: 'baseline',
        gap: 10
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 11px var(--font-mono)',
        color: 'var(--fg-4)'
      }
    }, "state"), /*#__PURE__*/React.createElement("span", {
      style: {
        font: '500 16px var(--font-mono)',
        color: 'var(--accent-fg)'
      }
    }, s)), /*#__PURE__*/React.createElement("div", {
      style: {
        display: 'flex',
        gap: 8,
        alignItems: 'center'
      }
    }, /*#__PURE__*/React.createElement("span", {
      style: {
        font: '400 11px var(--font-mono)',
        color: 'var(--fg-4)'
      }
    }, "send"), events[s].map(e => /*#__PURE__*/React.createElement(Tag, {
      key: e,
      glyph: "\u2192",
      style: {
        cursor: 'pointer'
      },
      onClick: () => setS(next[e])
    }, e)))))));
  }
  window.Hero = Hero;
})();
})(); } catch (e) { __ds_ns.__errors.push({ path: "ui_kits/website/Hero.jsx", error: String((e && e.message) || e) }); }

__ds_ns.WIRE_KINDS = __ds_scope.WIRE_KINDS;

__ds_ns.ArchDiagram = __ds_scope.ArchDiagram;

__ds_ns.Button = __ds_scope.Button;

__ds_ns.Icon = __ds_scope.Icon;

__ds_ns.IconButton = __ds_scope.IconButton;

__ds_ns.Kbd = __ds_scope.Kbd;

__ds_ns.Badge = __ds_scope.Badge;

__ds_ns.Card = __ds_scope.Card;

__ds_ns.CodeBlock = __ds_scope.CodeBlock;

__ds_ns.Sheet = __ds_scope.Sheet;

__ds_ns.Tag = __ds_scope.Tag;

__ds_ns.Dialog = __ds_scope.Dialog;

__ds_ns.Toast = __ds_scope.Toast;

__ds_ns.Tooltip = __ds_scope.Tooltip;

__ds_ns.Checkbox = __ds_scope.Checkbox;

__ds_ns.Input = __ds_scope.Input;

__ds_ns.Radio = __ds_scope.Radio;

__ds_ns.Select = __ds_scope.Select;

__ds_ns.Switch = __ds_scope.Switch;

__ds_ns.ExplodedLayout = __ds_scope.ExplodedLayout;

__ds_ns.MailboxStack = __ds_scope.MailboxStack;

__ds_ns.FIELD_KINDS = __ds_scope.FIELD_KINDS;

__ds_ns.MemoryLayout = __ds_scope.MemoryLayout;

__ds_ns.RingBuffer = __ds_scope.RingBuffer;

__ds_ns.Tabs = __ds_scope.Tabs;

__ds_ns.SpanTree = __ds_scope.SpanTree;

})();
