"""
Document renderer — converts PDF, images, and text to 1-bit PBM,
then to ZjStream (HP LaserJet 1020 native format) via foo2zjs.
"""

import io
import subprocess
from pathlib import Path

import fitz  # PyMuPDF
from PIL import Image


def render_pdf_to_pbm(pdf_bytes: bytes, dpi: int = 600,
                      page_range: str = "all",
                      orientation: str = "portrait",
                      paper: str = "letter") -> list[bytes]:
    """Render a PDF to a list of PBM P4 binary pages."""
    doc = fitz.open(stream=pdf_bytes, filetype="pdf")
    pages_to_render = _parse_page_range(page_range, len(doc))

    result = []
    for pg_num in pages_to_render:
        page = doc[pg_num]
        mat = fitz.Matrix(dpi / 72.0, dpi / 72.0)

        if orientation == "landscape":
            mat = mat.prerotate(90)

        pix = page.get_pixmap(matrix=mat, alpha=False)
        img = Image.frombytes("RGB", (pix.width, pix.height), pix.samples)
        pbm = _image_to_pbm(img, paper, dpi)
        result.append(pbm)

    doc.close()
    return result


def render_image_to_pbm(image_bytes: bytes, dpi: int = 600,
                        orientation: str = "portrait",
                        paper: str = "letter") -> list[bytes]:
    """Render a raster image to a single PBM page."""
    img = Image.open(io.BytesIO(image_bytes))
    if orientation == "landscape":
        img = img.rotate(-90, expand=True)
    pbm = _image_to_pbm(img, paper, dpi)
    return [pbm]


def render_text_to_pbm(text: str, dpi: int = 600,
                       paper: str = "letter") -> list[bytes]:
    """Render plain text to PBM pages via a temporary PDF."""
    doc = fitz.open()
    w, h = _paper_size_pt(paper)

    lines = text.split("\n")
    fontsize = 10
    margin = 50
    line_height = fontsize * 1.4
    lines_per_page = int((h - 2 * margin) / line_height)

    for chunk_start in range(0, max(len(lines), 1), lines_per_page):
        page = doc.new_page(width=w, height=h)
        chunk = lines[chunk_start:chunk_start + lines_per_page]
        y = margin + fontsize
        for line in chunk:
            page.insert_text((margin, y), line, fontsize=fontsize, fontname="helv")
            y += line_height

    pdf_bytes = doc.tobytes()
    doc.close()
    return render_pdf_to_pbm(pdf_bytes, dpi=dpi, paper=paper)


def _image_to_pbm(img: Image.Image, paper: str, dpi: int) -> bytes:
    """Convert a PIL image to PBM P4 binary, fitted to paper size."""
    target_w, target_h = _paper_size_px(paper, dpi)

    img = img.convert("L")  # grayscale

    scale = min(target_w / img.width, target_h / img.height)
    new_w = int(img.width * scale)
    new_h = int(img.height * scale)
    img = img.resize((new_w, new_h), Image.LANCZOS)

    canvas = Image.new("L", (target_w, target_h), 255)
    offset_x = (target_w - new_w) // 2
    offset_y = (target_h - new_h) // 2
    canvas.paste(img, (offset_x, offset_y))

    bw = canvas.point(lambda x: 0 if x < 128 else 255, "1")

    buf = io.BytesIO()
    bw.save(buf, format="PPM")
    return buf.getvalue()


def _paper_size_pt(paper: str) -> tuple[float, float]:
    sizes = {"letter": (612, 792), "a4": (595.28, 841.89)}
    return sizes.get(paper.lower(), sizes["letter"])


def _paper_size_px(paper: str, dpi: int) -> tuple[int, int]:
    pt_w, pt_h = _paper_size_pt(paper)
    return int(pt_w * dpi / 72), int(pt_h * dpi / 72)


def convert_pbm_to_zjs(pbm_bytes: bytes, dpi: int = 600,
                       paper: str = "letter") -> bytes:
    """Convert PBM P4 to ZjStream for HP LaserJet 1020 using foo2zjs."""
    header_end = pbm_bytes.index(b'\n', 3)
    dims = pbm_bytes[3:header_end].decode('ascii').split()
    width, height = int(dims[0]), int(dims[1])

    paper_code = {"letter": 1, "a4": 9, "legal": 26}.get(paper.lower(), 1)

    result = subprocess.run(
        ['foo2zjs',
         '-r', f'{dpi}x{dpi}',
         '-g', f'{width}x{height}',
         '-p', str(paper_code),
         '-z', '1'],
        input=pbm_bytes, capture_output=True, timeout=120,
    )
    if result.returncode != 0:
        raise RuntimeError(f"foo2zjs failed: {result.stderr.decode()}")
    return result.stdout


def _parse_page_range(spec: str, total: int) -> list[int]:
    if not spec or spec.lower() == "all":
        return list(range(total))

    pages = set()
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            start, end = part.split("-", 1)
            s = max(int(start) - 1, 0)
            e = min(int(end), total)
            pages.update(range(s, e))
        else:
            p = int(part) - 1
            if 0 <= p < total:
                pages.add(p)
    return sorted(pages)
