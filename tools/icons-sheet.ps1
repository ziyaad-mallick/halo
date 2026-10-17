# Contact sheet of build/icons from icons_test: for each app, BEFORE (the shell's image) and
# AFTER (what Halo loads), on a dark background. Writes build/icons-sheet.png.
Add-Type -AssemblyName System.Drawing
$dir = Join-Path (Split-Path $PSScriptRoot) 'build\icons'
$after = Get-ChildItem $dir -Filter '*.png' | Where-Object { $_.Name -notlike '*.before.png' } | Sort-Object Name
$cell = 112; $label = 22; $cols = 4
$w = $cols * ($cell * 2 + 12); $rows = [math]::Ceiling($after.Count / $cols)
$sheet = New-Object System.Drawing.Bitmap $w, ($rows * ($cell + $label))
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear([System.Drawing.Color]::FromArgb(32, 32, 32))
$g.InterpolationMode = 'HighQualityBicubic'
$font = New-Object System.Drawing.Font 'Segoe UI', 8
for ($i = 0; $i -lt $after.Count; $i++) {
    $x0 = ($i % $cols) * ($cell * 2 + 12); $y0 = [math]::Floor($i / $cols) * ($cell + $label)
    $g.DrawString(($after[$i].BaseName -replace '^\d+-', '') + '   before | after', $font, [System.Drawing.Brushes]::White, $x0 + 2, $y0 + 2)
    $before = Join-Path $dir ($after[$i].BaseName + '.before.png')
    foreach ($k in 0, 1) {
        $f = if ($k -eq 0) { $before } else { $after[$i].FullName }
        $img = [System.Drawing.Image]::FromFile($f)
        $g.DrawImage($img, $x0 + $k * $cell, $y0 + $label, $cell - 4, $cell - 4); $img.Dispose()
    }
}
$g.Dispose(); $p = Join-Path (Split-Path $dir) 'icons-sheet.png'; $sheet.Save($p); $sheet.Dispose(); $p
