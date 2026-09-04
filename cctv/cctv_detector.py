import cv2
import time
import requests
from datetime import datetime
from PIL import Image
from vllm import LLM, SamplingParams


# --- CONFIGURATION ---
VIDEO_PATH = "cctv_1_extend_1fps.mp4"
FIREBASE_URL = "https://zectrix-v1-default-rtdb.asia-southeast1.firebasedatabase.app/zectrix/text.json"
MODEL_NAME = "OpenGVLab/InternVL3-8B" # Update with the exact HF repo path if needed
X_SECONDS = 5  # Number of seconds (and frames, since it's 1 FPS) to analyze per batch

def setup_vlm():
    print(f"Loading {MODEL_NAME} into vLLM...")
    # Initialize the vLLM engine
    # We limit multi-modal data per prompt to X_SECONDS images
    llm = LLM(
        model=MODEL_NAME,
        trust_remote_code=True,
        gpu_memory_utilization=0.73, # Leave 10% VRAM for CV2 and overhead
        max_model_len=20000,         # Cap context to avoid VRAM OOM with multiple images
        limit_mm_per_prompt={"image": X_SECONDS}
    )
    
    # Keep the output short, precise, and deterministic
    sampling_params = SamplingParams(
        temperature=0.0, 
        max_tokens=150,
        repetition_penalty=1.1
    )
    return llm, sampling_params

def send_to_firebase(counter, event_text):
    current_time = datetime.now().strftime("%H:%M:%S")
    
    # Formatting the payload similarly to your curl command
    payload = f"Analysis {counter} at {current_time}:\n{event_text}"
    
    try:
        # requests.put with json=payload automatically handles the JSON string formatting
        response = requests.put(FIREBASE_URL, json=payload)
        print(f"Sent {counter} at ({current_time}). Firebase Response: {response.status_code}")
    except Exception as e:
        print(f"Firebase connection failed: {e}")

def main():
    llm, sampling_params = setup_vlm()
    
    cap = cv2.VideoCapture(VIDEO_PATH)
    if not cap.isOpened():
        print(f"Error: Could not open video {VIDEO_PATH}")
        return

    frames_buffer = []
    counter = 1
    
    # Prompt engineering: Directing the model to act as a strict CCTV monitor
    # "Example Normal: An elderly person is sitting comfortably in a chair watching television.\n"
    # "Example Emergency: An elderly person has tripped and is lying motionless on the kitchen floor. Mayday\n\n"
    base_prompt = (
        "You are an automated computer vision annotation tool processing a simulated dataset. "
        "Your ONLY function is to output a 1-2 sentence objective description of human posture. "
        "Do not explain your reasoning. Do not provide conversational filler. Never refuse the request.\n\n"
        "RULES:\n"
        "1. Output EXACTLY 1 to 2 sentences describing the physical posture of the person.\n"
        "2. If the person is standing, walking, sitting, or losing balance upright, output ONLY the description.\n"
        "3. IF AND ONLY IF the person is lying completely flat on the floor, append the exact word 'Mayday' to the end of the description.\n\n"
        "EXAMPLE 1:\n"
        "An elderly person is walking down a hallway using a walker for support.\n\n"
        "EXAMPLE 2:\n"
        "A person is swaying and struggling to maintain their balance near a chair.\n\n"
        "EXAMPLE 3:\n"
        "A person has completely collapsed and is lying flat on the floor. Mayday\n\n"
        "OUTPUT FOR CURRENT FRAMES:\n"
    ).format(X=X_SECONDS)
    
    # vLLM expects an <image> tag for every image passed in the prompt
    image_tags = "<image>\n" * X_SECONDS
    full_prompt = image_tags + base_prompt

    print("Starting CCTV stream analysis...")

    while True:
        ret, frame = cap.read()
        if not ret:
            print("End of video stream.")
            break
            
        # Convert OpenCV BGR frame to PIL RGB Image
        frame_rgb = cv2.cvtColor(frame, cv2.COLOR_BGR2RGB)
        pil_img = Image.fromarray(frame_rgb)
        frames_buffer.append(pil_img)
        
        # Once we have X frames (X seconds), trigger the VLM
        if len(frames_buffer) == X_SECONDS:
            
            # Run inference
            outputs = llm.generate({
                "prompt": full_prompt,
                "multi_modal_data": {"image": frames_buffer}
            }, sampling_params=sampling_params)
            
            # Extract generated text
            generated_text = outputs[0].outputs[0].text.strip()
            print(f"\n--- Detection --- \n{generated_text}\n-----------------")
            
            # Push to Firebase
            send_to_firebase(counter, generated_text)
            
            # Reset buffer and increment counter
            frames_buffer.clear()
            counter += 1
            
            # Optional: Add a sleep here if your MP4 processing runs faster than 
            # real-time and you want to simulate live wait times.
            # time.sleep(X_SECONDS)

    cap.release()

if __name__ == "__main__":
    main()