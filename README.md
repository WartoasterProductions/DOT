# **D.O.T. Sign Message Board – User Manual**

## **1\. Getting Started**

### **Power and Charging**

* **Power Switch:**  
  The power switch is located at the rear of the computer bin, behind the lid. Flip the switch to turn the unit on or off. Be gentle, it is very small.   
* **Charging Port:**  
  The charging port is on the front side of the battery bin, beneath the lid. Plug in a MicroUSB cable here.  
* **Battery and Solar Panel:**  
  The battery, charger circuit, and solar panel are permanently connected. When the unit is on, the solar panel will help recharge the battery. The solar panel also unfolds for optimal sunlight exposure.

  ### **Physical Orientation**

* **Screen Rotation:**  
  The display can be lifted and twisted 90 degrees in either direction. Adjust the screen so that it stays in an upright position for clear viewing.  
* **Joystick Location:**  
  The navigational joystick is located at the rear of the computer bin (accessible through the lid). Use it to navigate menus and change settings.

  ---

  ## **2\. Web Interface Overview**

When the D.O.T. Sign is powered on, it creates its own WiFi network. Use your phone or tablet to connect and configure the device.

### **How to Connect on a Phone/Tablet:**

1. **Join the WiFi Network:**  
   Open your WiFi settings and connect to the network named **DOTsign**.  
2. **Open the Browser:**  
   Once connected, open your preferred web browser. The captive portal should load automatically. If not, enter the device’s IP address (displayed on the board) into your browser’s address bar.

   ### **Using the Web Interface:**

The web interface is organized into several tabs:

* **Text Display:**  
  View and update saved text messages that cycle on the board. You can edit up to four pages of text, each containing three lines of eight characters.  
* **WiFi Setup:**  
  Scan for nearby networks, choose your WiFi network, and enter the password to connect the board to your home or office network. You can also delete stored credentials if you need to change networks.  
* **Connections:**  
  Set up weather and time settings:  
  * **OpenWeather API Key:**  
    To get current weather updates, you need an API key from OpenWeather:  
    * Visit [openweathermap.org](https://openweathermap.org/), sign up for a free account, and generate your API key.  
  * **Coordinates:**  
    To display weather correctly, enter your latitude and longitude. Use [latlong.net](https://www.latlong.net/) to find your coordinates:  
    * Enter your address or landmark, then copy the latitude and longitude values.  
  * **Timeserver & Time Zone:**  
    The default timeserver is **time.nist.gov**. You can adjust this along with your time zone as needed.  
* **Remote Control:**  
  Quickly change the board’s mode (for example, switch between Display, Clock, Weather, or Game modes) using on-screen buttons. You can also use the directional controls provided on the page to emulate the physical joystick. Good luck playing Snake. I mean it. I can’t get higher than 10 points on the remote   
* **Text Input:**  
  In this tab, you can directly type a new message. Simply enter your text in the provided text area and submit it; your message will then be saved and displayed on the board. Pasting too much text will crash the device and require you to enter **RESET** in the menu. Just don’t try and paste the Bee Movie script and it should be okay. 

  ---

  ## **3\. Navigation Using the Onboard Joystick**

In addition to the web interface, you can control the D.O.T. Sign directly with the navigational joystick:

* **Joystick Functions:**  
  * **Up/Down:**  
    Cycle through menu options or adjust display brightness.  
  * **Left/Right:**  
    Change pages when viewing messages or move the cursor in edit mode.  
  * **Select:**  
    Use this button to confirm selections or open the menu.  
* **Editing Text:**  
  When in Edit Mode, you can scroll through characters (using Up/Down) and use Right to confirm a character while Left acts as backspace. Your changes are saved immediately. You also have to be crazy to use this when the web interface exists.  
* **Menu Navigation:**  
  Pressing Select opens a menu where you can choose different operating modes (such as Clock, Weather, or Games). Once you make a selection, the board immediately switches modes.

  ---

  ## **4\. Additional Features**

  ### **Weather Display**

The board retrieves weather information from OpenWeather. When configured, it shows:

* **Temperature**  
* **Chance of Precipitation**  
* **Humidity**

  ### **Clock and Animations**

* **Analog Clock:**  
  The board can display an analog clock with a circular face, hour/minute/second hands, and tick marks.  
* **DVD Screensaver & Games:**  
  The DVD screensaver is obligatory. As was Snake.

  ### **Remote Control**

You can change modes and send directional commands remotely via the web interface. This is especially useful when you’re away from the device but still want to update its content.

