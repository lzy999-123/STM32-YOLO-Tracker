$header = Get-Content 'header_fix.txt' -Raw -Encoding UTF8
$code = Get-Content 'mainwindow.cpp' -Raw -Encoding UTF8
$idx = $code.IndexOf("void DnnThread::run() {")
if ($idx -ge 0) {
    $newCode = $header + $code.Substring($idx)
    Set-Content 'mainwindow.cpp' -Value $newCode -Encoding UTF8
} else {
    Write-Host "Could not find run()"
}
