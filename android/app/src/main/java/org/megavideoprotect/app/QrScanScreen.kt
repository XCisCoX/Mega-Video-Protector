package org.megavideoprotect.app

import android.app.Activity
import android.content.pm.ActivityInfo
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawingPadding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.compose.ui.viewinterop.AndroidView
import com.google.zxing.BarcodeFormat
import com.journeyapps.barcodescanner.BarcodeCallback
import com.journeyapps.barcodescanner.BarcodeResult
import com.journeyapps.barcodescanner.BarcodeView
import com.journeyapps.barcodescanner.DefaultDecoderFactory
import java.util.concurrent.atomic.AtomicBoolean

/** Portrait camera with a square QR frame. Not the barcode scanner. */
@Composable
fun QrScanScreen(onCode: (String) -> Unit, onClose: () -> Unit) {
    val context = LocalContext.current
    val activity = context as? Activity
    val handed = remember { AtomicBoolean(false) }

    DisposableEffect(activity) {
        val previous = activity?.requestedOrientation ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        activity?.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
        onDispose { activity?.requestedOrientation = previous }
    }

    Box(Modifier.fillMaxSize().background(Color.Black)) {
        AndroidView(
            modifier = Modifier.fillMaxSize(),
            factory = { ctx ->
                BarcodeView(ctx).apply {
                    decoderFactory = DefaultDecoderFactory(listOf(BarcodeFormat.QR_CODE))
                    framingRectSize = com.journeyapps.barcodescanner.Size(720, 720)
                    decodeContinuous(object : BarcodeCallback {
                        override fun barcodeResult(result: BarcodeResult?) {
                            val text = result?.text?.trim().orEmpty()
                            if (text.isEmpty() || !handed.compareAndSet(false, true)) return
                            pause()
                            onCode(text)
                        }

                        override fun possibleResultPoints(resultPoints: List<com.google.zxing.ResultPoint>?) = Unit
                    })
                    post { resume() }
                }
            },
        )
        ScanFrame()
        Text(
            "Scan QR Code From PC",
            color = Color.White,
            fontSize = 17.sp,
            modifier = Modifier.align(Alignment.Center).offset(y = 168.dp),
        )
        Text(
            "Close",
            color = Color.White,
            fontSize = 16.sp,
            modifier = Modifier
                .align(Alignment.TopStart)
                .safeDrawingPadding()
                .padding(start = 18.dp, top = 8.dp)
                .clickable(onClick = onClose),
        )
    }
}

@Composable
private fun ScanFrame() {
    Canvas(Modifier.fillMaxSize()) {
        val side = 260.dp.toPx()
        val left = (size.width - side) / 2f
        val top = (size.height - side) / 2f - 24.dp.toPx()
        val dim = Color.Black.copy(alpha = 0.55f)
        drawRect(dim, Offset(0f, 0f), Size(size.width, top.coerceAtLeast(0f)))
        drawRect(dim, Offset(0f, top + side), Size(size.width, (size.height - top - side).coerceAtLeast(0f)))
        drawRect(dim, Offset(0f, top), Size(left.coerceAtLeast(0f), side))
        drawRect(dim, Offset(left + side, top), Size((size.width - left - side).coerceAtLeast(0f), side))

        val color = Color.White
        val stroke = 4.dp.toPx()
        val arm = 32.dp.toPx()
        fun corner(x: Float, y: Float, right: Boolean, down: Boolean) {
            val dx = if (right) arm else -arm
            val dy = if (down) arm else -arm
            drawLine(color, Offset(x, y), Offset(x + dx, y), stroke, cap = StrokeCap.Square)
            drawLine(color, Offset(x, y), Offset(x, y + dy), stroke, cap = StrokeCap.Square)
        }
        corner(left, top, right = true, down = true)
        corner(left + side, top, right = false, down = true)
        corner(left, top + side, right = true, down = false)
        corner(left + side, top + side, right = false, down = false)
    }
}
