# Tiramisu dashboard preview

This is the standalone dashboard design recovered from the saved Tiramisu
project export. It includes the controller navigation, library, settings,
and game and mod views from that export. Preview actions change sample
data in the browser. The app's live console controls are served by the
Tiramisu UWP host.

Run the preview locally with Node.js 22.12 or newer.

    npm ci
    npm run dev

Run the type, logic, and production build checks.

    npm run lint
    npm test
    npm run build
