import 'esp-web-tools/dist/install-button.js';
const picker = document.querySelector('#firmware-model');
const installer = document.querySelector('esp-web-install-button');
if (picker && installer) picker.addEventListener('change', () => {
  installer.manifest = picker.value;
  installer.setAttribute('manifest',picker.value);
  document.querySelector('#firmware-selected').textContent = picker.selectedOptions[0].dataset.description;
});
