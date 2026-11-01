$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$taskIconDirectory = Join-Path $PSScriptRoot 'Assets'
$taskSourcePath = Join-Path $taskIconDirectory 'WireSteam.generated.png'
if (-not [IO.File]::Exists($taskSourcePath)) {
	throw "Generated icon artwork is missing: $taskSourcePath"
}

# This script only resizes the generated artwork and packs native ICO frames.
# Keep the image-generator output untouched so app, window, and tray share one mark.
$taskImages = @()
$taskSource = [Drawing.Bitmap]::new($taskSourcePath)
try {
	foreach ($taskSize in @(16, 20, 24, 32, 40, 48, 64, 128, 256)) {
		$taskBitmap = [Drawing.Bitmap]::new($taskSize, $taskSize)
		try {
			$taskGraphics = [Drawing.Graphics]::FromImage($taskBitmap)
			try {
				$taskStream = [IO.MemoryStream]::new()

				try {
					$taskGraphics.Clear([Drawing.Color]::Transparent)
					$taskGraphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
					$taskGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
					$taskGraphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
					$taskScale = [Math]::Min($taskSize / [double]$taskSource.Width, $taskSize / [double]$taskSource.Height)
					$taskWidth = [single]($taskSource.Width * $taskScale)
					$taskHeight = [single]($taskSource.Height * $taskScale)
					$taskTarget = [Drawing.RectangleF]::new([single](($taskSize - $taskWidth) / 2), [single](($taskSize - $taskHeight) / 2), $taskWidth, $taskHeight)
					$taskGraphics.DrawImage($taskSource, $taskTarget)
					$taskBitmap.Save($taskStream, [Drawing.Imaging.ImageFormat]::Png)
					$taskImages += , @($taskSize, $taskStream.ToArray())
				} finally {
					$taskStream.Dispose()
				}
			} finally {
				$taskGraphics.Dispose()
			}
		} finally {
			$taskBitmap.Dispose()
		}
	}
} finally {
	$taskSource.Dispose()
}

$taskOutput = [IO.File]::Create((Join-Path $taskIconDirectory 'WireSteam.ico'))

try {
	$taskWriter = [IO.BinaryWriter]::new($taskOutput)

	try {
		$taskWriter.Write([uint16]0x00)
		$taskWriter.Write([uint16]0x01)
		$taskWriter.Write([uint16]$taskImages.Count)
		$taskOffset = 0x06 + 0x10 * $taskImages.Count
		foreach ($taskImage in $taskImages) {
			$taskDimension = if ($taskImage[0] -eq 256) {
				0x00
			} else {
				$taskImage[0]
			}

			$taskWriter.Write([byte]$taskDimension)
			$taskWriter.Write([byte]$taskDimension)
			$taskWriter.Write([byte]0x00)
			$taskWriter.Write([byte]0x00)
			$taskWriter.Write([uint16]1)
			$taskWriter.Write([uint16]32)
			$taskWriter.Write([uint32]$taskImage[1].Length)
			$taskWriter.Write([uint32]$taskOffset)
			$taskOffset += $taskImage[1].Length
		}

		foreach ($taskImage in $taskImages) {
			$taskWriter.Write([byte[]]$taskImage[1])
		}
	} finally {
		$taskWriter.Dispose()
	}
} finally {
	$taskOutput.Dispose()
}
