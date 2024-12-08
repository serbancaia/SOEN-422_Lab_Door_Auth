#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <ESP32Servo.h>
//#include <Keypad.h>
#include <ArduinoJson.h>
#include <NTPClient.h>

#define SECONDS_IN_A_DAY 86400  // 24 * 60 * 60 = 86400 seconds in a day
#define MILLISECONDS_IN_A_SECOND 1000
#define MILLISECONDS_IN_A_MINUTE (60 * MILLISECONDS_IN_A_SECOND)
#define MILLISECONDS_IN_AN_HOUR (60 * MILLISECONDS_IN_A_MINUTE)

// WiFi Configuration
const char* ssid = "<Placeholder>";           // Replace with your WiFi SSID
const char* password = "<Placeholder>";   // Replace with your WiFi Password
const char* credServerUrl = "192.168.27.16"; // Replace with your server's domain or IP
const char* doorLogsServerUrl = "192.168.27.16";
const int httpsPort = 443;                // HTTPS port
const int httpsPort2 = 8443;
WiFiClientSecure client;

// NTP Client Setup
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 0, 60000); // UTC time

// Device info
String deviceID = "H-801"; // Replace with the meaningful label for this TTGO device

// Pins
/*
const int RED_LED_PIN = 13;
const int GREEN_LED_PIN = 12;
const int BUZZER_PIN = 15;
*/

// Pin assignments
const int servoPin = 15;
const int doorStatusIRPin = 25;
const int closestToDoorTripwirePin = 13;
const int farthestFromDoorTripwirePin = 12;
const int openDoorFromInButtonPin = 35;

/*
// Keypad Pins
const int r1 = 36;
const int r2 = 39;
const int r3 = 34;
const int r4 = 19;

const int c1 = 21;
const int c2 = 22;
const int c3 = 23;
//const int c4 = 16;

// Keypad Setup
const byte ROWS = 4;
const byte COLS = 3;
char keys[ROWS][COLS] = {
  {'1', '2', '3'},
  {'4', '5', '6'},
  {'7', '8', '9'},
  {'*', '0', '#'}
};
byte rowPins[ROWS] = {r1, r2, r3, r4};
byte colPins[COLS] = {c1, c2, c3};
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);
*/
enum TripwireSensingStates { 
  TW_Idle, TW_Active, TW_MaintenanceRequired
};

TripwireSensingStates tw_State = TW_Idle;

enum DoorStates { 
  D_Locked, D_ClosedUnlock, D_OpenedUnlock, D_MaintenanceRequired
};

DoorStates door_State = D_Locked;

enum MainBehaviourStates {
  Idle,
  WaitingConfirmation, 
  WaitingPassword, 
  FailedPasswordInputAttempt, 
  Unlocked,
  SelfMaintenanceWiFi, 
  SelfMaintenanceBluetooth, 
  MaintenanceRequired
};

MainBehaviourStates currentState = Idle;

// Timeout variables
const unsigned int rfidInputTimerLimit = 10000;
const unsigned long confirmationTimerLimit = 120000;  // 2 minutes
const unsigned int passcodeInputTimerLimit = 60000;
const unsigned int failedPasswordStateTimerLimit = 2000;

unsigned int rfidInputTimer = 0;
unsigned int confirmationTimer = 0;
unsigned int passcodeInputTimer = 0;
unsigned int failedPasswordStateTimer = 0;

unsigned int countFailedPasscodes = 0;

String enteredRFID = "";

// Password variables
String enteredOTP = "";
String correctOTP = "";

const int keypadButtonPin = 36;  // First button pin (to select the digit)
const int confirmButtonPin = 39; // Second button pin (to confirm the digit)
int buttonState1 = 0;      // Button 1 state
int buttonState2 = 0;      // Button 2 state
int lastButtonState1 = 0;  // Last state of button 1
int lastButtonState2 = 0;  // Last state of button 2
int selectedDigit = -1;     // Current selected digit

// Threshold values
const int doorStatusIRThreshold = 3000; // Door status IR threshold
const int closestToDoorTripwireThreshold = 3000; // Closest IR tripwire threshold
const int farthestFromDoorTripwireThreshold = 1900; // Farthest IR tripwire threshold

// State machine variables
int nbStudentCount = 0; // Number of students inside the room
unsigned char selfMaintenanceComplete = 0; // Binary flag announcing when the Bluetooth pairing with the WiFi-enabled TTGO is complete

// 2-bit variables stating the last and current state of the set of tripwires (00 = idle, 01 = closest high, 10 = farthest high, 11 = both high)
unsigned char lastTripwireComboState = 0;
unsigned char currentTripwireComboState = 0;

// 1-bit variable stating the last door state (0 = closed, 1 = open)
unsigned char lastDoorStatusState = 0;

// Mutually exclusive (apart when both are equal 0) binary variables announcing where a student emerged from
unsigned char isStudentEntering = 0; // Student entering state
unsigned char isStudentExiting = 0; // Student exiting state

// IR sensor binary values (either high (1) or low (0) based on their respective threshold)
unsigned char doorStatusIRValue = 0;
unsigned char closestToDoorTripwireValue = 0;
unsigned char farthestFromDoorTripwireValue = 0;

// Mutually exclusive (except when both equal to 0) binary variables saying whether someone unlocked the door via passcode or from the inside of the lab room
unsigned char unlockedFromOutside = 0;
unsigned char unlockedFromInside = 0;
unsigned char requestUnlockFromOutside = 0;

// Timer to count down the number of milliseconds since the door has been unlocked from the one who entered a password
int closedUnlockTimer = 0;
// Limit for the above timer
int closedUnlockTimerLimit = 15000;

// Timer to count down the number of milliseconds since the door has been opened
int openedUnlockTimer = 0;
// Limit for the above timer
int openedUnlockTimerLimit = 15000;

// Servo setup
Servo doorLockServo;

void setup() {
  Serial.begin(9600);

  // Initialize pins
  pinMode(doorStatusIRPin, INPUT);
  pinMode(closestToDoorTripwirePin, INPUT);
  pinMode(farthestFromDoorTripwirePin, INPUT);
  pinMode(openDoorFromInButtonPin, INPUT_PULLUP);
  pinMode(keypadButtonPin, INPUT_PULLUP);  // Set keypad button pin as input with pull-up resistor
  pinMode(confirmButtonPin, INPUT_PULLUP); // Set confirm button pin as input with pull-up resistor

  pinMode(openDoorFromInButtonPin, INPUT_PULLUP);
  
  // Setup servo
  doorLockServo.attach(servoPin);
  doorLockServo.write(0); // Initially locked (position 0)

  // Connect to WiFi
  connectToWiFi();
  timeClient.begin();
}

void loop() {
  doorStatusIRValue = analogRead(doorStatusIRPin) >= doorStatusIRThreshold;
  closestToDoorTripwireValue = analogRead(closestToDoorTripwirePin) >= closestToDoorTripwireThreshold;
  farthestFromDoorTripwireValue = analogRead(farthestFromDoorTripwirePin) >= farthestFromDoorTripwireThreshold;

  //Serial.println(doorStatusIRValue);
  //Serial.println(closestToDoorTripwireValue);
  //Serial.println(farthestFromDoorTripwireValue);
  delay(100);

  // Door State machine logic
  doorSM();

  // Tripwire Sensing State machine logic
  tripwireSensingSM();
  
  // Main system behaviour logic
  mainBehaviourSM();
}

void mainBehaviourSM() {
  switch (currentState) {
    case Idle:
      handleIdleState();
      break;

    case WaitingConfirmation:
      handleWaitingConfirmationState();
      break;

    case WaitingPassword:
      handleWaitingPasswordState();
      break;

    case FailedPasswordInputAttempt:
      handleFailedPasswordInputAttemptState();
      break;

    case Unlocked:
      handleUnlockedState();
      break;

    case SelfMaintenanceWiFi:
      handleSelfMaintenanceWiFiState();
      break;

    case SelfMaintenanceBluetooth:
      handleSelfMaintenanceBluetoothState();
      break;

    case MaintenanceRequired:
      handleMaintenanceRequiredState();
      break;
  }
}

void tripwireSensingSM() {
  switch(tw_State) {
    case TW_Idle:
      if (farthestFromDoorTripwireValue) {
        isStudentExiting = 1;
        currentTripwireComboState = 2;
        tw_State = TW_Active;
      }
      else if (doorStatusIRValue && closestToDoorTripwireValue) {
        isStudentEntering = 1;
        currentTripwireComboState = 1;
        tw_State = TW_Active;
      }
      break;
    case TW_Active:
      if (currentTripwireComboState ^ ((farthestFromDoorTripwireValue << 1) | closestToDoorTripwireValue)) {
        lastTripwireComboState = currentTripwireComboState;
        currentTripwireComboState = ((farthestFromDoorTripwireValue << 1) | closestToDoorTripwireValue);
        if (currentTripwireComboState == 0) {
          switch(lastTripwireComboState){
            case 1:
              decrementNbStudentCount();
              break;
            case 2:
              incrementNbStudentCount();
              break;
          }
          isStudentEntering = 0;
          isStudentExiting = 0;
          lastTripwireComboState = 0;
          tw_State = TW_Idle;
          break;
        }
      }
      if (doorStatusIRValue ^ lastDoorStatusState) {
        lastDoorStatusState = doorStatusIRValue;
      }
      break;
    case TW_MaintenanceRequired:
      // Stay in MaintenanceRequired if an error persists (e.g., malfunction)
      break;
  }
}

void doorSM() {
  switch(door_State) {
    case D_Locked:
      if (digitalRead(openDoorFromInButtonPin) == HIGH) { // if unlocked from inside
        unlockedFromInside = 1;
        unlockedFromOutside = 0;
        unlockDoor();  // Unlock the door if button pressed
        sendResponseToLogServer("Door unlocked from the inside");
        Serial.println("Door unlocked from the inside");
        door_State = D_ClosedUnlock;  // Transition to ClosedUnlock state
        currentState = Unlocked;
      } else if (requestUnlockFromOutside) {
        unlockedFromOutside = 1;
        unlockedFromInside = 0;
        unlockDoor();  // Unlock the door if correct passcode given
        sendResponseToLogServer("Door unlocked via passcode");
        Serial.println("Door unlocked via passcode");
        door_State = D_ClosedUnlock;  // Transition to ClosedUnlock state
        currentState = Unlocked;
      }
      break;

    case D_ClosedUnlock:
      if (digitalRead(openDoorFromInButtonPin) == LOW) { // if no one inside is currently pushing the door handle down (i.e. button)
        if (doorStatusIRValue) {
          unlockedFromOutside = 0;
          unlockedFromInside = 0;
          closedUnlockTimer = 0;
          sendResponseToLogServer("Door opened after being unlocked by inside door handle");
          Serial.println("Door opened after being unlocked by inside door handle");
          door_State = D_OpenedUnlock;  // Transition to OpenedUnlock if the door opens
          break;
        } else if (unlockedFromInside) {
          lockDoor();
          sendResponseToLogServer("Door locked automatically after door shut");
          Serial.println("Door locked automatically after door shut");
          door_State = D_Locked; // Lock the door if the person who opened the door was from inside and isn't keeping the door open nor holding down on the door handle
          correctOTP = "";
          enteredOTP = "";
          enteredRFID = "";
          currentState = Idle;
          break;
        }
      }
      if (doorStatusIRValue) {
        unlockedFromOutside = 0;
        unlockedFromInside = 0;
        closedUnlockTimer = 0;
        Serial.println("Door opened");
        sendResponseToLogServer("Door opened");
        door_State = D_OpenedUnlock;  // Transition to OpenedUnlock if the door opens
        break;
      }
      if (unlockedFromOutside) {
        if (closedUnlockTimer < closedUnlockTimerLimit) {
          closedUnlockTimer = closedUnlockTimer + 100;
        } else {
          if (digitalRead(openDoorFromInButtonPin) == LOW) {
            lockDoor();
            sendResponseToLogServer("Door locked after 15 seconds because outsider took too long to open it");
            Serial.println("Door locked after 15 seconds because outsider took too long to open it");
            door_State = D_Locked; // Lock the door if outside person who inputted a correct passcode took more than 15 seconds to open the door
            correctOTP = "";
            enteredOTP = "";
            enteredRFID = "";
            currentState = Idle;
            break;
          }
        }
      }
      break;

    case D_OpenedUnlock:
      if (doorStatusIRValue == 0) {
        if (digitalRead(openDoorFromInButtonPin) == LOW) {
          lockDoor(); // Lock the door if the doorStatusIR sensor value falls below the threshold
          sendResponseToLogServer("Door locked automatically after door shut");
          Serial.println("Door locked automatically after door shut");
          door_State = D_Locked;
          correctOTP = "";
          enteredOTP = "";
          enteredRFID = "";
          currentState = Idle;
          break;
        } else {
          openedUnlockTimer = 0;
          unlockedFromInside = 1;
          door_State = D_ClosedUnlock;
          sendResponseToLogServer("Door closed but is still unlocked via door handle from inside the lab room");
          Serial.println("Door closed but is still unlocked via door handle from inside the lab room");
          break;
        }
      } else {
        if (openedUnlockTimer >= openedUnlockTimerLimit) {
          Serial.println("Door has been kept opened for " + String(openedUnlockTimer) + " seconds. It's far too long!");
          sendResponseToLogServer("Door has been kept opened for " + String(openedUnlockTimer) + " seconds. It's far too long!");
        }
        openedUnlockTimer = openedUnlockTimer + 100;
      }
      break;

    case D_MaintenanceRequired:
      // Stay in MaintenanceRequired if an error persists (e.g., malfunction)
      break;
  }
}

void handleIdleState() {
  if (millis() - rfidInputTimer < rfidInputTimerLimit) {
    // Keypad Implementation
    /*
    char key = keypad.getKey();
    if (key) {
      handleKeypadInputForRFID(key);
    }
    */            
    // 2 Buttons Implementation                                                                                                                                            
    buttonState1 = digitalRead(keypadButtonPin);  // Read the state of the first button
    buttonState2 = digitalRead(confirmButtonPin);  // Read the state of the second button
    handleDigitSelectionForRFID();  // Handle the digit selection logic
    handleConfirmationForRFID();     // Handle the confirmation logic
  } else {
    enteredRFID = "";
    rfidInputTimer = 0;
  }
}

/*
void handleKeypadInputForRFID(char key) {
  if (key == '#') {  // Submit OTP
    sendRequestToCredentialServer(enteredRFID);
    rfidInputTimer = 0;
    currentState = WaitingConfirmation;
    sendResponseToLogServer("RFID scanned. Sending it to credentials server for user authentication");
  } else if (key == '*') {
    enteredRFID = "";
  } else if (key == '0' || key == '1' || key == '2' || key == '3' || key == '4' || key == '5' || key == '6' || key == '7' || key == '8' || key == '9') {
    enteredRFID += key;
    Serial.println(enteredRFID);
  }
}
*/

void handleDigitSelectionForRFID() {
  // Check if the first button is pressed
  if (buttonState1 == LOW && lastButtonState1 == HIGH) {
    selectedDigit++;  // Increment the selected digit
    if (selectedDigit > 9) {  // Reset to 0 after 9
      selectedDigit = 0;
    }
    Serial.print("Selected digit: ");
    Serial.println(selectedDigit);
    delay(300);  // Small delay to debounce the button press
  }

  lastButtonState1 = buttonState1;  // Save the current state of button 1
}

// Method to handle the confirmation of the selected digit
void handleConfirmationForRFID() {
  // Check if the second button is pressed
  if (buttonState2 == LOW && lastButtonState2 == HIGH) {
    if (selectedDigit != -1) {
      appendDigitToRFID();  // If a digit was selected, append it to the sequence
    } else {
      if (enteredRFID != "") {
        submitRFID();  // Submit the sequence if no new digit is selected
      }
    }
    delay(300);  // Small delay to debounce the button press
  }

  lastButtonState2 = buttonState2;  // Save the current state of button 2
}

// Method to append the selected digit to the sequence
void appendDigitToRFID() {
  enteredRFID += String(selectedDigit);  // Append the selected digit
  Serial.print("Current RFID: ");
  Serial.println(enteredRFID);  // Show the updated sequence
  selectedDigit = -1;  // Reset the selected digit for next input
  delay(300);  // Small delay after appending
}

// Method to submit the sequence when confirmed
void submitRFID() {
  Serial.println("RFID submitted: " + enteredRFID);
  sendRequestToCredentialServer(enteredRFID);
  rfidInputTimer = 0;
  currentState = WaitingConfirmation;
  buttonState1 = 0;      // Button 1 state
  buttonState2 = 0;      // Button 2 state
  lastButtonState1 = 0;  // Last state of button 1
  lastButtonState2 = 0;  // Last state of button 2
  selectedDigit = -1;     // Current selected digit
  sendResponseToLogServer("RFID " + enteredRFID + " scanned. Sending it to credentials server for user authentication");
  Serial.println("RFID " + enteredRFID + " scanned. Sending it to credentials server for user authentication");
}

void handleDigitSelectionForPassword() {
  // Check if the first button is pressed
  if (buttonState1 == LOW && lastButtonState1 == HIGH) {
    selectedDigit++;  // Increment the selected digit
    if (selectedDigit > 9) {  // Reset to 0 after 9
      selectedDigit = 0;
    }
    Serial.print("Selected digit: ");
    Serial.println(selectedDigit);
    delay(300);  // Small delay to debounce the button press
  }

  lastButtonState1 = buttonState1;  // Save the current state of button 1
}

// Method to handle the confirmation of the selected digit
void handleConfirmationForPassword() {
  // Check if the second button is pressed
  if (buttonState2 == LOW && lastButtonState2 == HIGH) {
    if (selectedDigit != -1) {
      appendDigitToPassword();  // If a digit was selected, append it to the sequence
    } else {
      if (enteredRFID != "") {
        submitPassword();  // Submit the sequence if no new digit is selected
      }
    }
    delay(300);  // Small delay to debounce the button press
  }

  lastButtonState2 = buttonState2;  // Save the current state of button 2
}

// Method to append the selected digit to the sequence
void appendDigitToPassword() {
  enteredOTP += String(selectedDigit);  // Append the selected digit
  Serial.print("Current OTP: ");
  Serial.println(enteredOTP);  // Show the updated sequence
  selectedDigit = -1;  // Reset the selected digit for next input
  delay(300);  // Small delay after appending
}

// Method to submit the sequence when confirmed
void submitPassword() {
  Serial.println("OTP submitted: " + enteredOTP);
  if (enteredOTP == correctOTP) {
    passcodeInputTimer = 0;
    countFailedPasscodes = 0;
    currentState = Unlocked;
    door_State = D_ClosedUnlock;
    sendResponseToLogServer("Correct password given. Unlock attempt succeeded");
  } else {
    if (countFailedPasscodes < 3) {
      enteredOTP = "";
      countFailedPasscodes = countFailedPasscodes + 1;
      sendResponseToLogServer("Failed password attempt " + String(countFailedPasscodes) + " times");
    } else {
      sendResponseToLogServer("Failed password attempt 3 times. Unlock attempt failed");
      currentState = FailedPasswordInputAttempt;
    }
  }
  buttonState1 = 0;      // Button 1 state
  buttonState2 = 0;      // Button 2 state
  lastButtonState1 = 0;  // Last state of button 1
  lastButtonState2 = 0;  // Last state of button 2
  selectedDigit = -1;     // Current selected digit
}

void handleWaitingConfirmationState() {
  if (millis() - confirmationTimer < confirmationTimerLimit) {
    if (client.available()) {
      String response = client.readString();
      
      // Check response for confirmation
      if (response.indexOf("\"status\":\"confirmed\"") != -1) {
        // Find the start of the JSON body (after headers)
        int jsonStart = response.indexOf("{");
        if (jsonStart == -1) {
          Serial.println("Invalid response format: No JSON found.");
          confirmationTimer = 0;
          enteredRFID = "";
          correctOTP = "";
          enteredOTP = "";
          currentState = Idle;
          sendResponseToLogServer("Invalid credentials server response format: No JSON found.");
        }

        String jsonResponse = response.substring(jsonStart);

        // Parse JSON using ArduinoJson
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, jsonResponse);

        if (error) {
          Serial.print("JSON deserialization failed: ");
          Serial.println(error.c_str());
          confirmationTimer = 0;
          enteredRFID = "";
          correctOTP = "";
          enteredOTP = "";
          currentState = Idle;
          sendResponseToLogServer("JSON deserialization failed at credentials server response");
        }

        // Check whether response belongs to this device and for the given rfid
        if (doc.containsKey("request_id")) {
          if (String(doc["request_id"]).startsWith(deviceID + "-" + enteredRFID + "-")) {
            // Extract the OTP field
            if (doc.containsKey("OTP")) {
              correctOTP = doc["OTP"].as<String>();
              Serial.println("Extracted OTP: " + correctOTP);
              confirmationTimer = 0;
              sendResponseToLogServer("Student successfully authenticated themselves. One-Time Password was provided to the keypad");
              currentState = WaitingPassword;
            } else {
              Serial.println("No OTP found in response.");
              enteredRFID = "";
              confirmationTimer = 0;
              correctOTP = "";
              enteredOTP = "";
              currentState = Idle;
              sendResponseToLogServer("No One-Time Password was provided by credentials server");
            }
          }
        }
      }
      client.stop(); // Close connection after confirmation
    }
  } else {
    enteredRFID = "";
    confirmationTimer = 0;
    currentState = Idle;
    correctOTP = "";
    enteredOTP = "";
    sendResponseToLogServer("Outsider took too long to respond to their authentication message. Authentication failed");
  }
}

void handleWaitingPasswordState() {
  if (millis() - passcodeInputTimer < passcodeInputTimerLimit) {
    /*char key = keypad.getKey();
    if (key) {
      handleKeypadInputForPasscode(key);
    }
    */
    handleDigitSelectionForPassword();
    handleConfirmationForPassword();
  } else {
    currentState = FailedPasswordInputAttempt;
    sendResponseToLogServer("Outsider took more than 30 seconds to input the correct password. Unlock attempt failed");
  }
}
/*
void handleKeypadInputForPasscode(char key) {
  if (key == '#') {  // Submit OTP
    if (enteredOTP == correctOTP) {
      passcodeInputTimer = 0;
      countFailedPasscodes = 0;
      currentState = Unlocked;
      door_State = D_ClosedUnlock;
      correctOTP = "";
      enteredOTP = "";
      sendResponseToLogServer("Correct password given. Unlock attempt succeeded");
    } else {
      if (countFailedPasscodes < 3) {
        enteredOTP = "";
        countFailedPasscodes = countFailedPasscodes + 1;
        sendResponseToLogServer("Failed password attempt " + String(countFailedPasscodes) + " times");
      } else {
        sendResponseToLogServer("Failed password attempt 3 times. Unlock attempt failed");
        currentState = FailedPasswordInputAttempt;
      }
    }
  } else if (key == '*') {  // Reset OTP
    enteredOTP = "";
  } else if (key == '0' || key == '1' || key == '2' || key == '3' || key == '4' || key == '5' || key == '6' || key == '7' || key == '8' || key == '9') {
    enteredOTP += key;
    Serial.println(enteredOTP);
  }
}
*/
void handleFailedPasswordInputAttemptState() {
  if (millis() - failedPasswordStateTimer < failedPasswordStateTimerLimit) {
    // Nothing happens
  } else {
    rfidInputTimer = 0;
    confirmationTimer = 0;
    passcodeInputTimer = 0;
    failedPasswordStateTimer = 0;
    countFailedPasscodes = 0;
    enteredRFID = "";
    enteredOTP = "";
    correctOTP = "";
    currentState = Idle;
  }
}

void handleUnlockedState() {
  // Nothing happens
}

void handleSelfMaintenanceWiFiState() {
}

void handleSelfMaintenanceBluetoothState() {
}

void handleMaintenanceRequiredState() {
}

void incrementNbStudentCount() {
  if (isStudentEntering) {
    nbStudentCount++;
    Serial.println("Student entered. Current count: " + String(nbStudentCount));
    sendResponseToLogServer("Student entered. Current count: " + String(nbStudentCount));
  } else {
    if (nbStudentCount > 0) {
      Serial.println("Student attempted to leave but went back inside. Current count: " + String(nbStudentCount));
      sendResponseToLogServer("Student attempted to leave but went back inside. Current count: " + String(nbStudentCount));
    }
  }
}

void decrementNbStudentCount() {
  if (lastDoorStatusState && isStudentExiting) {
    if (nbStudentCount > 0) {
      nbStudentCount--;
      Serial.println("A student left the room. Current count: " + String(nbStudentCount));
      sendResponseToLogServer("A student left the room. Current count: " + String(nbStudentCount));
    }
  } else {
    Serial.println("A student attempted to enter but left. Current count: " + String(nbStudentCount));
    sendResponseToLogServer("A student left the room. Current count: " + String(nbStudentCount));
  }
}

void unlockDoor() {
  doorLockServo.write(90); // Unlock (position 90)
  Serial.println("Door unlocked!");
  requestUnlockFromOutside = 0;
}

void lockDoor() {
  doorLockServo.write(0); // Lock (position 0)
  Serial.println("Door locked!");
  unlockedFromOutside = 0;
  unlockedFromInside = 0;
  closedUnlockTimer = 0;
  openedUnlockTimer = 0;
}

// Function to send request to the credential server
void sendRequestToCredentialServer(String rfid) {
  client.setInsecure(); // Use only for testing with self-signed certificates
  if (!client.connect(credServerUrl, httpsPort)) {
    //Serial.println("Connection to credential server failed.");
    currentState = MaintenanceRequired;
    door_State = D_MaintenanceRequired;
    tw_State = TW_MaintenanceRequired;
  }

  // Construct JSON payload
  String payload = "{";
  payload += "\"rfid\": \"" + rfid + "\",";
  payload += "\"device_id\": \"" + deviceID + "\"";
  payload += "}";

  // Construct HTTP POST request
  String request = "POST /rfid HTTP/1.1\r\n";
  request += "Host: " + String(credServerUrl) + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(payload.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += payload;

  // Send the request
  client.print(request);
  Serial.println("Request sent to credential server.");
}

void sendResponseToLogServer(String response) {
  client.setInsecure(); // Use only for testing with self-signed certificates
  if (!client.connect(doorLogsServerUrl, httpsPort2)) {
    //Serial.println("Connection to door log server failed.");
    currentState = MaintenanceRequired;
    door_State = D_MaintenanceRequired;
    tw_State = TW_MaintenanceRequired;
  }

  // Construct JSON payload
  String payload = "{";
  payload += "\"device_id\": \"" + deviceID + "\",";
  payload += "\"message\": \"" + response + "\",";
  payload += "\"timestamp\": \"" + createTimestamp() + "\"";
  payload += "}";

  // Construct HTTP POST request
  String request = "POST /log HTTP/1.1\r\n";
  request += "Host: " + String(doorLogsServerUrl) + "\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + String(payload.length()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += payload;

  // Send the request
  client.print(request);
  Serial.println("Response sent to door log server.");
}

String createTimestamp() {
  // Get the current time since the program started
  unsigned long currentMillis = millis();
  
  // Calculate time in seconds from milliseconds
  unsigned long totalSeconds = currentMillis / MILLISECONDS_IN_A_SECOND;
  
  // Get the current day, hour, minute, and second

  // Format the timestamp: YYYY-MM-DD, HH:MM:SS
  return String("2024-12-") + ((totalSeconds / SECONDS_IN_A_DAY) + 1) + " at " + ((totalSeconds % SECONDS_IN_A_DAY) / (60 * 60)) + ":" + ((totalSeconds % (60 * 60)) / 60) + ":" + (totalSeconds % 60);
}

// Function to connect to WiFi
void connectToWiFi() {
  //Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nWiFi connected!");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
}
