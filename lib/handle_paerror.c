// Add implementation for handle_paerror function
static void handle_paerror(PaError err, char * where)
{
    if (err != paNoError) {
        PORT_LOG("%s: PortAudio error: %s", where, Pa_GetErrorText(err));
        error_count++;
        
        if (error_count > 20) {
            PORT_LOG("%s: Too many PortAudio errors (%d), audio quality may be degraded", 
                    where, error_count);
            
            // Log a user-visible message for persistent audio issues
            if (error_count % 10 == 0) { // Only show every 10th error
                iaxci_usermsg(IAXC_TEXT_TYPE_NOTICE, 
                    "Audio system experiencing issues. You may need to restart the application if audio quality degrades.");
            }
        }
    }
}
