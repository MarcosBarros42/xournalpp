# <img src="ui/pixmaps/com.github.xournalpp.xournalpp.svg" align="left" width="100" height="100"> Ournal++ Drive Sync

**Fork do [Xournal++](https://github.com/xournalpp/xournalpp) com sincronização nativa no Google Drive — Versão [v1.0](#v10)**

*Ournal++ Drive Sync* é um fork próprio do [Xournal++](https://xournalpp.github.io), software de anotação à
mão escrito em C++ (GTK3), adicionando a ele um módulo completo de sincronização com o Google Drive,
integrado direto ao app — sem ferramentas externas.

<p align="center">
  <img src="readme/main.png" alt="Xournal++ em ação" width="600"/>
</p>

---

## ✨ O que este fork adiciona

A branch [`feature/drive-sync`](https://github.com/MarcosBarros42/xournalpp/tree/feature/drive-sync) traz:

| Funcionalidade | Descrição |
| --- | --- |
| 🔐 **Login OAuth 2.0** | Autenticação com conta Google pelo próprio app (app instalado / loopback redirect) |
| ♻️ **Renovação automática de token** | `refresh_token` renovado e salvo nas configurações, com expiração gerenciada |
| ☁️ **Sincronização no Drive** | Sobe o `.xopp` para uma pasta do Drive (criando ou atualizando o arquivo) |
| 🖥️ **Painel integrado nas Preferências** | Status da conexão, botões de Login / Atualizar / Sincronizar / Logout |
| 📁 **Escopo mínimo** | Usa apenas `drive.file` — o app só enxerga os arquivos que ele mesmo criou |

### Primeiro uso: configurando as credenciais

1. No [Google Cloud Console](https://console.cloud.google.com/apis/credentials), crie um projeto e um
   **ID do cliente OAuth 2.0** do tipo **App para computador**, com a **Google Drive API** ativada.
2. Crie o arquivo `cloudclient.json` na pasta de configurações do app
   (no Windows: `%LOCALAPPDATA%\xournalpp\cloudclient.json`); o caminho correto aparece na tela de
   Preferências → Cloud.
3. Preencha no formato:

   ```json
   {
     "project": "meu-projeto",
     "created": "2026-10-21",
     "type": "authorized_user_desktop_installed_app",
     "client_id": "1234567890-xxxx.apps.googleusercontent.com",
     "client_secret": "GOCSPX-xxxxxxxxxxxx",
     "oauth_scope": "https://www.googleapis.com/auth/drive.file",
     "redirect_flow": "http://127.0.0.1 ephemeral port (installed-app loopback flow)"
   }
   ```

4. Ativo o app → **Preferências → Cloud → Login com Google Drive** → pronto: enquanto o app
   estiver conectado, qualquer documento aberto sincroniza ao clicar em **Sincronizar agora**.

> 💡 Segurança: o `client_secret` nunca deve ser publicado. Se ele vazar, redefina o secret no
> Console e atualize o `cloudclient.json`.

---

## 🚀 Versão v1.0

Primeira versão estável da sincronização. O que está pronto:

- ✍️ Criação de arquivo novo no Drive (upload multipart) na pasta do usuário fixada no app
- 🔀 Atualização de conteúdo de arquivo já existente (`PATCH .../upload/drive/v3/files/{id}?uploadType=media`)
- 🔑 Login OAuth completo com renovação automática de access token
- 🧰 `windows-setup/package.sh` monta pasta **dist autocontida** para Windows (DLLs do MSYS2,
  loaders GTK, ícones, traduções e **bundle de CA** para o curl funcionar fora do ambiente de desenvolvimento)
- 🐞 Correções feitas durante a v1.0:
  - URL de update duplicava `files/` (`.../drive/v3/files/files/{id}`)
  - `PUT` não é aceito pelo endpoint de update do Drive → trocado por `PATCH`
  - Bundde de certificados CA agora é resolvido a partir da pasta do próprio exe (`cacert.pem`)

---

## 🔨 Compilar no Windows (MSYS2 UCRT64 ambiente)

```bash
pacman -S mingw-w64-ucrt-x86_64-toolchain \
          mingw-w64-ucrt-x86_64-cmake \
          mingw-w64-ucrt-x86_64-ninja \
          mingw-w64-ucrt-x86_64-gtk3 \
          libucrt64 (usuário passa os demais pacotes: json-glib, curl, poppler, portaudio, gtksourceview4 etc.)
```

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -GNinja
cmake --build build
bash windows-setup/package.sh build   # gera build/dist (pasta portátil) e tentaria criar o instalador NSIS
```

Para outros sistemas, veja as instruções do upstream
([Linux](readme/LinuxBuild.md), [Windows](readme/WindowsBuild.md), [macOS](readme/MacBuild.md)).

---

## 📚 Créditos

Projeto base: [Xournal++](https://github.com/xournalpp/xournalpp) — um excelente software livre de
anotação à mão (GPLv2). Este fork mantém os créditos e a licença originais, e adiciona o módulo de
nuvem (`src/core/control/cloud/`, `src/core/gui/dialog/DriveSettingsPanel.*`, `ui/cloudSettings.glade`).
