import os
import io
import sys

threshold = 128
USAGE = (
    'Usage: python scripts/convert_icon.py input.png|input.svg output_name width height\n'
    '   or: python scripts/convert_icon.py --manifest file --svgdir dir --size N --out header.h\n'
    '\n'
    'Manifest mode packs several icons into one header, one `ArrayName = svg-stem` per line\n'
    '(# comments allowed). Same transform as single-icon mode, so the arrays go to\n'
    'GfxRenderer::drawIcon just the same.'
)

def svg_to_png_bytes(svg_path, width, height):
    import cairosvg

    with open(svg_path, 'rb') as f:
        svg_data = f.read()
    png_bytes = cairosvg.svg2png(bytestring=svg_data, output_width=width, output_height=height)
    return png_bytes

def load_image(path, width, height):
    from PIL import Image

    ext = os.path.splitext(path)[1].lower()
    if ext == '.svg':
        png_bytes = svg_to_png_bytes(path, width, height)
        img = Image.open(io.BytesIO(png_bytes)).convert('RGBA')
        # Flatten alpha, same as the raster branch: a stroke-only SVG renders on transparency,
        # and converting that straight to L gives 0 everywhere -- a solid black tile.
        background = Image.new('RGBA', img.size, (255, 255, 255, 255))
        background.paste(img, mask=img.split()[3])
        img = background
    else:
        img = Image.open(path)
        img = img.convert('RGBA')
        img = img.resize((width, height), Image.LANCZOS)
        # Flatten alpha: paste on white background
        background = Image.new('RGBA', img.size, (255, 255, 255, 255))
        background.paste(img, mask=img.split()[3])
        img = background
    # Rotate 90 degrees counterclockwise
    img = img.rotate(90, expand=True)
    return img

def pack_bits(img):
    # Convert to grayscale, then threshold to get white=1, black=0
    img = img.convert('L')
    width, height = img.size
    pixels = list(img.getdata())
    packed = []
    for y in range(height):
        for x in range(0, width, 8):
            byte = 0
            for b in range(8):
                if x + b < width:
                    v = pixels[y * width + x + b]
                    # 1 for white, 0 for black
                    bit = 1 if v >= threshold else 0
                    byte |= (bit << (7 - b))
            packed.append(byte)
    return packed, width, height


def format_array(packed, width, height, array_name):
    c = f'// size: {width}x{height}\n'
    c += f'static const uint8_t {array_name}[] = {{\n    '
    for i, v in enumerate(packed):
        c += f'0x{v:02X}, '
        if (i + 1) % 16 == 0:
            c += '\n    '
    return c.rstrip(', \n') + '\n};\n'


def image_to_c_array(img, array_name):
    packed, width, height = pack_bits(img)
    return '#pragma once\n#include <cstdint>\n\n' + format_array(packed, width, height, array_name)


def parse_manifest(path):
    entries = []
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        name, _, source = line.partition('=')
        source = source.strip() or name.strip()
        source, _, op = source.partition('+')
        entries.append((name.strip(), source.strip(), op.strip()))
    return entries


def unpack_bits(packed, size):
    """Inverse of pack_bits: 1 = ink."""
    return [[0 if (packed[y * (size // 8) + x // 8] >> (7 - x % 8)) & 1 else 1
             for x in range(size)] for y in range(size)]


def repack_bits(grid, size):
    packed = []
    for row in grid:
        for xb in range(0, size, 8):
            byte = 0
            for b in range(8):
                x = xb + b
                byte |= (0 if x < size and row[x] else 1) << (7 - b)
            packed.append(byte)
    return packed


def derive_fill(grid, size):
    """Solidify enclosed white: flood the background in from the border, ink the rest.

    The filled counterpart of a closed outline (a house, a box). An open outline leaks and comes
    out solid black, so check the result before using this on one."""
    outside = [[False] * size for _ in range(size)]
    stack = [(y, x) for i in range(size) for (y, x) in ((0, i), (size - 1, i), (i, 0), (i, size - 1))]
    while stack:
        y, x = stack.pop()
        if not (0 <= y < size and 0 <= x < size) or outside[y][x] or grid[y][x]:
            continue
        outside[y][x] = True
        stack += [(y + 1, x), (y - 1, x), (y, x + 1), (y, x - 1)]
    return [[1 if grid[y][x] or not outside[y][x] else 0 for x in range(size)] for y in range(size)]


def derive_bold(grid, size):
    """Dilate the ink by one pixel: the filled counterpart of a stroke-drawn glyph (a shelf of
    books), where solidifying the enclosed gaps would merge the whole thing into a block."""
    return [[1 if any(grid[y + dy][x + dx]
                      for dy in (-1, 0, 1) for dx in (-1, 0, 1)
                      if 0 <= y + dy < size and 0 <= x + dx < size) else 0
             for x in range(size)] for y in range(size)]


DERIVATIONS = {'fill': derive_fill, 'bold': derive_bold}


def read_array(spec):
    """`@path/to/header.h:ArrayName` -- an icon already packed in the tree, so a variant can be
    derived from the committed asset instead of re-tracing its source art."""
    import re

    path, _, name = spec[1:].partition(':')
    match = re.search(r'\b%s\[\] = \{(.*?)\};' % re.escape(name), open(path).read(), re.S)
    if not match:
        print(f'ERROR: {name} not found in {path}', file=sys.stderr)
        sys.exit(1)
    return [int(v, 16) for v in re.findall(r'0x([0-9A-Fa-f]{2})', match.group(1))]


def run_manifest(manifest, svgdir, size, out):
    parts = ['#pragma once', '#include <cstdint>', '',
             f'// Generated by scripts/convert_icon.py from {svgdir} via {manifest}. Do not edit.', '']
    for array_name, source, op in parse_manifest(manifest):
        if source.startswith('@'):
            packed, width, height = read_array(source), size, size
        else:
            svg = os.path.join(svgdir, source + '.svg')
            if not os.path.exists(svg):
                print(f'ERROR: missing svg for {array_name}: {svg}', file=sys.stderr)
                sys.exit(1)
            packed, width, height = pack_bits(load_image(svg, size, size))
        if op:
            if op not in DERIVATIONS:
                print(f'ERROR: unknown derivation "+{op}" for {array_name}', file=sys.stderr)
                sys.exit(1)
            packed = repack_bits(DERIVATIONS[op](unpack_bits(packed, size), size), size)
        parts.append(format_array(packed, width, height, array_name))
    open(out, 'w').write('\n'.join(parts))
    print(f'Wrote {out}')

def main():
    if any(arg in ('-h', '--help') for arg in sys.argv[1:]):
        print(USAGE)
        sys.exit(0)
    if '--manifest' in sys.argv:
        args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
        run_manifest(args['--manifest'], args['--svgdir'], int(args['--size']), args['--out'])
        return
    if len(sys.argv) != 5:
        print(USAGE)
        sys.exit(1)
    input_path, output_name, width, height = sys.argv[1:5]
    array_name = output_name.capitalize() + 'Icon'
    width, height = int(width), int(height)
    img = load_image(input_path, width, height)
    c_array = image_to_c_array(img, array_name)

    # Always save to src/components/icons/[output_name].h relative to project root
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_dir = os.path.join(project_root, 'src', 'components', 'icons')
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, f'{output_name}.h')
    with open(output_path, 'w') as f:
        f.write(c_array)
    print(f'Wrote {output_path}')

if __name__ == '__main__':
    main()
