// The module's bytes, from wherever the build put them.
//
// Served from the source tree, `url` is a path the dev server answers. In the
// built page it is a `data:` URL: the build inlines every asset, so the one
// file opens from a download with nothing serving it - and a data URL is
// decoded here rather than fetched, because a page opened from `file://`
// cannot rely on fetch at all.
export async function loadWasm(url) {
  const inline = /^data:[^,]*;base64,(.*)$/s.exec(url);
  if (inline) {
    const binary = atob(inline[1]);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i);
    return bytes;
  }
  const response = await fetch(url);
  if (!response.ok) throw new Error(`could not load the module (${url}: ${response.status})`);
  return response.arrayBuffer();
}
