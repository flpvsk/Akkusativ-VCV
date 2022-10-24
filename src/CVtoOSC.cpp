#include "plugin.hpp"

#include <memory>
#include <nonstd/optional.hpp>
#include <queue>
#include <exception>

#include <boost/bind/bind.hpp>
#include <boost/tuple/tuple.hpp>
#include <boost/asio.hpp>
#include <boost/array.hpp>

#include "OSCSender.cpp"

#define MICROS_PER_SEC         1000000
#define us2s(x) (((double)x)/(double)MICROS_PER_SEC)


timeval getCurrentTime() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv;
}


struct CVtoOSC : Module {
  rack::dsp::TTimer<float> timer;
  float lastReset = 0.f;

  std::string url;
  bool isUrlDirty = false;

  std::string address1;
  bool isAddress1Dirty = false;

  std::unique_ptr<OSCSender> oscSender;

	enum ParamId {
    SAMPLE_RATE_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		CV1_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		OUTPUTS_LEN
	};
	enum LightId {
		LIGHTS_LEN
	};

	CVtoOSC() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
    configParam(SAMPLE_RATE_PARAM, 0.00001f, 10.f, 0.001f, "Sample Rate", "s");
		configInput(CV1_INPUT, "CV1");
    oscSender = std::unique_ptr<OSCSender>(new OSCSender());
    oscSender->start();
	}

  void onReset(const ResetEvent& e) override {
    timer.reset();

    url = "";
    isUrlDirty = true;

    address1 = "";
    isAddress1Dirty = true;
  }

  void fromJson(json_t* rootJ) override {
    Module::fromJson(rootJ);
    json_t* urlJ = json_object_get(rootJ, "ip:port");
    if (urlJ) url = json_string_value(urlJ);
    isUrlDirty = true;

    json_t* address1J = json_object_get(rootJ, "address1");
    if (address1J) address1 = json_string_value(address1J);
    isAddress1Dirty = true;
  }

  json_t* dataToJson() override {
    json_t* rootJ = json_object();
    json_object_set_new(rootJ, "ip:port", json_stringn(url.c_str(), url.size()));
    json_object_set_new(
      rootJ,
      "address1",
      json_stringn(address1.c_str(), address1.size())
    );
    return rootJ;
  }

  void dataFromJson(json_t* rootJ) override {
    json_t* urlJ = json_object_get(rootJ, "ip:port");
    if (urlJ) url = json_string_value(urlJ);
    isUrlDirty = true;

    json_t* address1J = json_object_get(rootJ, "address1");
    if (address1J) address1 = json_string_value(address1J);
    isAddress1Dirty = true;
  }

  void onUrlUpdate(std::string newUrl) {
    DEBUG("on url update %s", newUrl.c_str());
    using boost::asio::ip::udp;
    using boost::asio::ip::address;
    using boost::asio::ip::make_address;

    url = newUrl;
    isUrlDirty = false;

    std::string ip;
    std::string portStr;
    bool hasIp = false;
    bool hasPort = false;

    int l = url.length();
    for (int i = 0; i < l; i++) {
      if (url[i] == ':' && hasIp) {
        hasPort = false;
        continue;
      }

      if (url[i] == ':' && !hasIp) {
        hasIp = true;
        continue;
      }

      if (hasIp) {
        portStr.push_back(url[i]);
        hasPort = true;
        continue;
      }

      ip.push_back(url[i]);
    }

    nonstd::optional<boost::asio::ip::udp::endpoint> endpoint;

    if (!hasPort || !hasIp) {
      DEBUG(
        "Malformed string '%s' hasIp? %d hasPort? %d",
        url.c_str(),
        hasIp,
        hasPort
      );
      endpoint = nonstd::nullopt;
      return;
    }

    address parsed;
    try {
      parsed = make_address(ip);
    } catch (const std::exception& e) {
      DEBUG("Address is wrong %s", ip.c_str());
      endpoint = nonstd::nullopt;
      return;
    }

    int port = 0;
    try {
      port = stoi(portStr);
    } catch (const std::exception& e) {
      DEBUG("Port is wrong %s", portStr.c_str());
      endpoint = nonstd::nullopt;
      return;
    }

    DEBUG("Endpoint created %s:%d", ip.c_str(), port);
    endpoint = udp::endpoint(parsed, port);
    oscSender->setEndpoint(std::move(endpoint.value()));
  }

  void onRemove(const RemoveEvent& e) override {
    oscSender->stop();
    oscSender.reset(nullptr);
    DEBUG("xxx onRemove done xxx");
  }

	void process(const ProcessArgs& args) override {
    float cv1 = clamp(
        inputs[CV1_INPUT].getVoltage(),
        0.f,
        12.f
    ) / 12.f;
    float sampleRateParam = params[SAMPLE_RATE_PARAM].getValue();

    timer.process(args.sampleTime);
    if (timer.getTime() < sampleRateParam) {
      // DEBUG("t %f, lr %f, sr %f", timer.getTime(), lastReset, sampleRateParam);
      return;
    }

    timer.reset();

    OSCMessage msg1;
    msg1.type = OSCMessage::FLOAT;
    msg1.f = cv1;
    msg1.address = address1;

    OSCMessage msg2;
    msg2.type = OSCMessage::FLOAT;
    msg2.f = 0.2;
    msg2.address = "u_speed";

    OSCBundle bundle;
    bundle.time = getCurrentTime();
    bundle.messages = new OSCMessage[2]{ msg1, msg2 };
    bundle.messagesSize = 2;

    oscSender->send(std::move(bundle));
	}
};

struct URLTextField: LedDisplayTextField {
  CVtoOSC* module;

  void step() override {
    LedDisplayTextField::step();
    if (!module || !module->isUrlDirty) return;
    module->onUrlUpdate(module->url);
    setText(module->url);
  }

  void onChange(const ChangeEvent& e) override {
    if (!module) return;
    module->onUrlUpdate(getText());
  }
};


struct URLDisplay: LedDisplay {
  void setModule(CVtoOSC* module) {
    URLTextField* textField = createWidget<URLTextField>(Vec(0, 0));
    textField->box.size = box.size;
    textField->multiline = false;
    textField->module = module;
    addChild(textField);
  }
};

struct AddressTextField: LedDisplayTextField {
  CVtoOSC* module;

  void step() override {
    LedDisplayTextField::step();
    if (!module || !module->isAddress1Dirty) return;
    setText(module->address1);
    module->isAddress1Dirty = false;
  }

  void onChange(const ChangeEvent& e) override {
    if (module) module->address1 = getText();
  }
};


struct AddressDisplay: LedDisplay {
  void setModule(CVtoOSC* module) {
    AddressTextField* textField = createWidget<AddressTextField>(Vec(0, 0));
    textField->box.size = box.size;
    textField->multiline = false;
    textField->module = module;
    addChild(textField);
  }
};


struct PortDisplay: widget::Widget {
	std::string fontPath;
	std::string bgText;
	std::string text;
	float fontSize;
	NVGcolor bgColor = nvgRGB(0x46,0x46, 0x46);
	NVGcolor fgColor = SCHEME_YELLOW;
	Vec textPos;

  PortDisplay() {
    fontPath = asset::system("res/fonts/DSEG7ClassicMini-BoldItalic.ttf");
    textPos = Vec(2, 4);
    bgText = "8888";
    fontSize = 16;
  }

	void prepareFont(const DrawArgs& args) {
		// Get font
		std::shared_ptr<Font> font = APP->window->loadFont(fontPath);
		if (!font)
			return;
		nvgFontFaceId(args.vg, font->handle);
		nvgFontSize(args.vg, fontSize);
		nvgTextLetterSpacing(args.vg, 0.0);
		nvgTextAlign(args.vg, NVG_ALIGN_TOP | NVG_ALIGN_LEFT);
	}

	void draw(const DrawArgs& args) override {
		// Background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, 0, 0, box.size.x, box.size.y);
		nvgFillColor(args.vg, nvgRGB(0x19, 0x19, 0x19));
		nvgFill(args.vg);

		prepareFont(args);

		// Background text
		nvgFillColor(args.vg, bgColor);
		nvgText(args.vg, textPos.x, textPos.y, bgText.c_str(), NULL);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer == 1) {
			prepareFont(args);

			// Foreground text
			nvgFillColor(args.vg, fgColor);
			nvgText(args.vg, textPos.x, textPos.y, text.c_str(), NULL);
		}
		Widget::drawLayer(args, layer);
	}
};

struct CVtoOSCWidget : ModuleWidget {
	CVtoOSCWidget(CVtoOSC* module) {
		setModule(module);
		setPanel(
      createPanel(asset::plugin(pluginInstance, "res/Akkusativ_CV_OSC.svg"))
    );

		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

    URLDisplay* urlDisplay = createWidget<URLDisplay>(Vec(0, 32));
    urlDisplay->box.size = Vec(150, 32);
    urlDisplay->setModule(module);
    addChild(urlDisplay);

    PortDisplay* portDisplay = createWidget<PortDisplay>(Vec(0, 76));
    portDisplay->box.size = Vec(150, 24);
    addChild(portDisplay);

    AddressDisplay* address1Display = createWidget<AddressDisplay>(Vec(0, 120));
    address1Display->box.size = Vec(150, 32);
    address1Display->setModule(module);
    addChild(address1Display);


		addInput(createInputCentered<PJ301MPort>(Vec(RACK_GRID_WIDTH, 164), module, CVtoOSC::CV1_INPUT));

    addParam(createParam<Trimpot>(Vec(RACK_GRID_WIDTH + 64, 164), module, CVtoOSC::SAMPLE_RATE_PARAM));
	}
};


Model* modelCVtoOSC = createModel<CVtoOSC, CVtoOSCWidget>("CVtoOSC");
