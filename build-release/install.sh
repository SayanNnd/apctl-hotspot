set -eu

archive_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
install_dir="${HOME}/.local/bin"

install -Dm755 "$archive_dir/apctl" "$install_dir/apctl"
printf 'Installed to %s/apctl\n' "$install_dir"

case ":$PATH:" in
  *":$install_dir:"*) ;;
  *) printf 'Add this to your shell config if apctl is not found:\nexport PATH="%s:$PATH"\n' "$install_dir" ;;
esac
