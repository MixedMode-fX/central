// The DOM, built rather than templated. Two functions and a convention:
// `el` for HTML, `svg` for SVG, each taking a tag, attributes and children.
// An attribute starting with `on` is a listener; a null, undefined or false
// attribute or child is nothing; a string child is text; an array child is
// spread.

const SVG_NS = 'http://www.w3.org/2000/svg';

function build(node, attrs, children) {
  for (const [key, value] of Object.entries(attrs)) {
    if (value === null || value === undefined || value === false) continue;
    if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
    else node.setAttribute(key, value);
  }
  for (const child of children.flat(Infinity)) {
    if (child === null || child === undefined || child === false) continue;
    node.append(child.nodeType ? child : document.createTextNode(String(child)));
  }
  return node;
}

export const el = (tag, attrs = {}, ...children) =>
  build(document.createElement(tag), attrs, children);

export const svg = (tag, attrs = {}, ...children) =>
  build(document.createElementNS(SVG_NS, tag), attrs, children);

// Class names from a list, with the falsy ones left out: `classes('param',
// inline && 'inline')` rather than a template literal full of ternaries.
export const classes = (...names) => names.flat().filter(Boolean).join(' ');

// Empty an element: the counterpart of `el`, for a panel written into rather
// than rebuilt.
export const clear = (node) => { while (node.firstChild) node.firstChild.remove(); };
