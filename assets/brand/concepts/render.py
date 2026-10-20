import sys, resvg_py
def render(svg, png, size):
    open(png,'wb').write(bytes(resvg_py.svg_to_bytes(svg_path=svg, width=size, height=size)))
if __name__=="__main__":
    render(sys.argv[1], sys.argv[2], int(sys.argv[3]))
