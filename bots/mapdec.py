"""Map decoding -- a port of data/map-decoder.js.

The wire format is produced by encodeCell() in hex-map.hpp.  This is a direct
port of the client's decoder rather than a re-derivation of the encoder, so
the bots see exactly what the browser sees, bug-for-bug.

  Full map ("sync".map)   6 hex chars per cell, row-major, MAP_ROWS x MAP_COLS
  Vis disk ("vis".cells) 10 hex chars per cell: QQ RR TT DD VV

  TT  terrain byte, 0xFF == fog (never revealed to this player)
      bits 0-5 terrain id, bit 6 improved shelter, bit 7 caravan tire track
  DD  bits 0-5 footprint bitmask (which players have stood here)
      bit 6 has shelter (any), bit 7 has POI
  VV  high nibble resource type (0 none, 1-5), low nibble art variant

Note the vis disk carries no `amount` -- the bot can see THAT a pile is on a
hex but not how big it is, same as the client.  Pile size arrives via the
EVT_COLLECT event once you step on it.
"""
from dataclasses import dataclass
from config import MAP_COLS, MAP_ROWS


@dataclass(slots=True)
class Cell:
    terrain: int
    footprints: int     # bitmask of player ids that have visited
    shelter: int        # 0 none, 1 basic, 2 improved
    poi: bool
    tire_track: bool
    resource: int       # 0 none, 1-5 (index into RES_NAME is resource-1)
    variant: int

    def visited_by(self, pid: int) -> bool:
        return bool(self.footprints & (1 << pid))


def decode_cell(tt: int, dd: int, vv: int) -> Cell | None:
    """Returns None for a fogged cell (TT == 0xFF)."""
    if tt == 0xFF:
        return None
    has_shelter = (dd >> 6) & 1
    return Cell(
        terrain=tt & 0x3F,
        footprints=dd & 0x3F,
        shelter=(2 if (tt & 0x40) else 1) if has_shelter else 0,
        poi=bool((dd >> 7) & 1),
        tire_track=bool((tt >> 7) & 1),
        resource=(vv >> 4) & 0xF,
        variant=vv & 0xF,
    )


class WorldMap:
    """Player-local map view.  Fogged cells stay None until revealed."""

    def __init__(self, rows: int = MAP_ROWS, cols: int = MAP_COLS):
        self.rows, self.cols = rows, cols
        self.grid: list[list[Cell | None]] = [[None] * cols for _ in range(rows)]

    def __getitem__(self, qr: tuple[int, int]) -> Cell | None:
        q, r = qr
        return self.grid[r % self.rows][q % self.cols]

    def load_full(self, hex_str: str) -> dict:
        """Parse a 'sync'.map payload.  Returns counts for logging."""
        revealed = fog = pois = shelters = resources = 0
        expect = self.rows * self.cols * 6
        if len(hex_str) < expect:
            # Short payload means the firmware's buffer truncated -- worth
            # knowing about rather than silently decoding garbage.
            raise ValueError(f"map payload {len(hex_str)} chars, expected {expect}")
        for r in range(self.rows):
            base = r * self.cols * 6
            row = self.grid[r]
            for c in range(self.cols):
                i = base + c * 6
                tt = int(hex_str[i:i + 2], 16)
                if tt == 0xFF:
                    row[c] = None
                    fog += 1
                    continue
                dd = int(hex_str[i + 2:i + 4], 16)
                vv = int(hex_str[i + 4:i + 6], 16)
                row[c] = decode_cell(tt, dd, vv)
                revealed += 1
                if (dd >> 7) & 1:   pois += 1
                if (dd >> 6) & 1:   shelters += 1
                if (vv >> 4) & 0xF: resources += 1
        return {"revealed": revealed, "fog": fog, "poi": pois,
                "shelter": shelters, "resource": resources}

    def apply_vis(self, cells: str) -> int:
        """Apply a 'vis'.cells disk.  Returns the number of cells updated."""
        n = 0
        for i in range(0, len(cells) - 9, 10):
            q  = int(cells[i:i + 2], 16)
            r  = int(cells[i + 2:i + 4], 16)
            tt = int(cells[i + 4:i + 6], 16)
            dd = int(cells[i + 6:i + 8], 16)
            vv = int(cells[i + 8:i + 10], 16)
            if r < self.rows and q < self.cols:
                self.grid[r][q] = decode_cell(tt, dd, vv)
                n += 1
        return n

    def known_cells(self):
        """Yield (q, r, cell) for every revealed cell."""
        for r in range(self.rows):
            for q, cell in enumerate(self.grid[r]):
                if cell is not None:
                    yield q, r, cell
