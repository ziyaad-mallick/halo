"""Headless regression for interruption-safe Start transitions (no preview)."""
import argparse
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--revision')
args = parser.parse_args()
source = (subprocess.check_output(['git', 'show', f'{args.revision}:src/launcher.cpp'], cwd=root).decode('utf-8')
          if args.revision else (root / 'src/launcher.cpp').read_text(encoding='utf-8'))
body = re.search(r'void Launcher::OpenMotion\(.*?^\}', source, re.M | re.S).group()
show = re.search(r'void Launcher::Show\(.*?^\}', source, re.M | re.S).group()
stop = re.search(r'void Launcher::StopServices\(.*?^\}', source, re.M | re.S).group()
before_direction = body[:body.index('    if (in) {')]
before_direction = re.sub(r'    if \(st.effect == settings::Effect::None\) \{.*?\n    \}', '', before_direction, flags=re.S)
# Stopping these first discards the compositor presentation pose. A close during
# entry then starts at the base value instead of continuing from the visible pose.
checks = {
    'close preserves active offset': 'v.StopAnimation(L"Offset")' not in before_direction,
    'close preserves active opacity': 'v.StopAnimation(L"Opacity")' not in before_direction,
    'opening has a bounded offset transition': 'slide.Duration(ms(motion::kFadeInMs))' in body,
    'close takes current presentation pose': 'slide.InsertKeyFrame(0.0f' not in body[body.index('    } else {\n        auto down'):],
    'device setup yields until after first layout': 'StartServices();' not in show and
        'SetTimer(m_hwnd, kServicesTimer' in show and show.index('Build();') < show.index('SetTimer(m_hwnd, kServicesTimer'),
    'closing cancels deferred device setup': 'KillTimer(m_hwnd, kServicesTimer)' in stop,
}
for name, ok in checks.items():
    print(('PASS ' if ok else 'FAIL ') + name)
print(f'{len(checks)} checks, {sum(not ok for ok in checks.values())} failures')
raise SystemExit(0 if all(checks.values()) else 1)
