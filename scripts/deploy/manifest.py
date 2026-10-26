"""Update the deployed release manifest for every file of dist/ that differs from it (tools/ excluded).
Writes release.json and changed.json (the list install.ps1 copies) into the folder given as argv[1]."""
import hashlib, json, os, sys
root = r'D:\Projects\Native Ports\Umbrella-chronicles-recomp'
out = sys.argv[1]
manifest = json.load(open(os.path.join(root, 'dist-raw', 'release.json')))
changed = []
for entry in manifest['files']:
    path = entry['path']
    if path.startswith('tools/'):
        continue
    source = os.path.join(root, 'dist', *path.split('/'))
    if not os.path.exists(source):
        continue
    data = open(source, 'rb').read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != entry['sha256']:
        entry['size'] = len(data)
        entry['sha256'] = digest
        changed.append(path)
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'release.json'), 'w', newline='\n').write(json.dumps(manifest, indent=2) + '\n')
json.dump(changed, open(os.path.join(out, 'changed.json'), 'w'))
print('changed:', changed)
